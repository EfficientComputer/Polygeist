#include "PassDetails.h"

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/OpenMP/OpenMPDialect.h"
#include "mlir/Dialect/SCF/Passes.h"
#include "mlir/Dialect/SCF/SCF.h"
#include "mlir/IR/BlockAndValueMapping.h"
#include "mlir/IR/Dominance.h"
#include "mlir/IR/Matchers.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "polygeist/Passes/Passes.h"
#include <mlir/Dialect/Arithmetic/IR/Arithmetic.h>

using namespace mlir;
using namespace mlir::func;
using namespace mlir::arith;
using namespace polygeist;

namespace {
struct OpenMPOpt : public OpenMPOptPassBase<OpenMPOpt> {
  void runOnOperation() override;
};
} // namespace

/// Merge any consecutive parallel's
///
///    omp.parallel {
///       codeA();
///    }
///    omp.parallel {
///       codeB();
///    }
///
///  becomes
///
///    omp.parallel {
///       codeA();
///       omp.barrier
///       codeB();
///    }
bool isReadOnly(Operation *op) {
  bool hasRecursiveEffects = op->hasTrait<OpTrait::HasRecursiveSideEffects>();
  if (hasRecursiveEffects) {
    for (Region &region : op->getRegions()) {
      for (auto &block : region) {
        for (auto &nestedOp : block)
          if (!isReadOnly(&nestedOp))
            return false;
      }
    }
  }

  // If the op has memory effects, try to characterize them to see if the op
  // is trivially dead here.
  if (auto effectInterface = dyn_cast<MemoryEffectOpInterface>(op)) {
    // Check to see if this op either has no effects, or only allocates/reads
    // memory.
    SmallVector<MemoryEffects::EffectInstance, 1> effects;
    effectInterface.getEffects(effects);
    if (!llvm::all_of(effects, [op](const MemoryEffects::EffectInstance &it) {
          return isa<MemoryEffects::Read>(it.getEffect());
        })) {
      return false;
    }
    return true;
  }
  return false;
}

struct CombineParallel : public OpRewritePattern<omp::ParallelOp> {
  using OpRewritePattern<omp::ParallelOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(omp::ParallelOp nextParallel,
                                PatternRewriter &rewriter) const override {
    Block *parent = nextParallel->getBlock();
    if (nextParallel == &parent->front())
      return failure();

    // Only attempt this if there is another parallel within the function, which
    // is not contained within this operation.
    bool noncontained = false;
    nextParallel->getParentOfType<FuncOp>()->walk([&](omp::ParallelOp other) {
      if (!nextParallel->isAncestor(other)) {
        noncontained = true;
      }
    });
    if (!noncontained)
      return failure();

    omp::ParallelOp prevParallel;
    SmallVector<Operation *> prevOps;

    bool changed = false;

    for (Operation *prevOp = nextParallel->getPrevNode(); 1;) {
      if (prevParallel = dyn_cast<omp::ParallelOp>(prevOp)) {
        break;
      }
      // We can move this into the parallel if it only reads
      if (isReadOnly(prevOp) &&
          llvm::all_of(prevOp->getResults(), [&](Value v) {
            return llvm::all_of(v.getUsers(), [&](Operation *user) {
              return nextParallel->isAncestor(user);
            });
          })) {
        auto prevIter =
            (prevOp == &parent->front()) ? nullptr : prevOp->getPrevNode();
        rewriter.setInsertionPointToStart(&nextParallel.getRegion().front());
        auto replacement = rewriter.clone(*prevOp);
        rewriter.replaceOp(prevOp, replacement->getResults());
        changed = true;
        if (!prevIter)
          return success();
        prevOp = prevIter;
        continue;
      }
      return success(changed);
    }

    rewriter.setInsertionPointToEnd(&prevParallel.getRegion().front());
    rewriter.replaceOpWithNewOp<omp::BarrierOp>(
        prevParallel.getRegion().front().getTerminator(), TypeRange());
    rewriter.mergeBlocks(&nextParallel.getRegion().front(),
                         &prevParallel.getRegion().front());
    rewriter.eraseOp(nextParallel);
    return success();
  }
};

struct ParallelForInterchange : public OpRewritePattern<omp::ParallelOp> {
  using OpRewritePattern<omp::ParallelOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(omp::ParallelOp nextParallel,
                                PatternRewriter &rewriter) const override {
    Block *parent = nextParallel->getBlock();
    if (parent->getOperations().size() != 2)
      return failure();

    auto prevFor = dyn_cast<scf::ForOp>(nextParallel->getParentOp());
    if (!prevFor || prevFor->getResults().size())
      return failure();

    nextParallel->moveBefore(prevFor);
    auto yield = nextParallel.getRegion().front().getTerminator();
    auto contents =
        rewriter.splitBlock(&nextParallel.getRegion().front(),
                            nextParallel.getRegion().front().begin());
    rewriter.mergeBlockBefore(contents, &prevFor.getBody()->front());
    rewriter.setInsertionPoint(prevFor.getBody()->getTerminator());
    rewriter.create<omp::BarrierOp>(nextParallel.getLoc());
    rewriter.setInsertionPointToEnd(&nextParallel.getRegion().front());
    auto newYield = rewriter.clone(*yield);
    rewriter.eraseOp(yield);
    prevFor->moveBefore(newYield);
    return success();
  }
};

struct ParallelIfInterchange : public OpRewritePattern<omp::ParallelOp> {
  using OpRewritePattern<omp::ParallelOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(omp::ParallelOp nextParallel,
                                PatternRewriter &rewriter) const override {
    Block *parent = nextParallel->getBlock();
    if (parent->getOperations().size() != 2)
      return failure();

    auto prevIf = dyn_cast<scf::IfOp>(nextParallel->getParentOp());
    if (!prevIf || prevIf->getResults().size())
      return failure();

    nextParallel->moveBefore(prevIf);
    auto yield = nextParallel.getRegion().front().getTerminator();
    auto contents =
        rewriter.splitBlock(&nextParallel.getRegion().front(),
                            nextParallel.getRegion().front().begin());
    rewriter.mergeBlockBefore(contents, &prevIf.getBody()->front());
    rewriter.setInsertionPointToEnd(&nextParallel.getRegion().front());
    auto newYield = rewriter.clone(*yield);
    rewriter.eraseOp(yield);
    prevIf->moveBefore(newYield);
    return success();
  }
};

void OpenMPOpt::runOnOperation() {
  mlir::RewritePatternSet rpl(getOperation()->getContext());
  rpl.add<CombineParallel, ParallelForInterchange, ParallelIfInterchange>(
      getOperation()->getContext());
  GreedyRewriteConfig config;
  config.maxIterations = 47;
  (void)applyPatternsAndFoldGreedily(getOperation(), std::move(rpl), config);
}

std::unique_ptr<Pass> mlir::polygeist::createOpenMPOptPass() {
  return std::make_unique<OpenMPOpt>();
}