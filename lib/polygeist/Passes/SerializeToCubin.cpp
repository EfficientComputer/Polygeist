//===- LowerGPUToCUBIN.cpp - Convert GPU kernel to CUBIN blob -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements a pass that serializes a gpu module into CUBIN blob and
// adds that blob as a string attribute of the module.
//
//===----------------------------------------------------------------------===//

#include "mlir/Dialect/GPU/Transforms/Passes.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/Support/raw_ostream.h"

#if POLYGEIST_ENABLE_CUDA
#include "mlir/Dialect/GPU/Transforms/Passes.h"
#include "mlir/ExecutionEngine/OptUtils.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Target/LLVMIR/Dialect/LLVMIR/LLVMToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Dialect/NVVM/NVVMToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Export.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Linker/Linker.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/Transforms/IPO/Internalize.h"

#include <cuda.h>
// TODO use this library if possible, crashes for some reason
#include <nvPTXCompiler.h>

#define DEBUG_TYPE "serialize-to-cubin"

using namespace mlir;

static void emitCudaError(const llvm::Twine &expr, const char *buffer,
                          CUresult result, Location loc) {
  const char *error;
  cuGetErrorString(result, &error);
  emitError(loc, expr.concat(" failed with error code ")
                     .concat(llvm::Twine{error})
                     .concat("[")
                     .concat(buffer)
                     .concat("]"));
}

#define RETURN_ON_CUDA_ERROR(expr)                                             \
  do {                                                                         \
    if (auto status = (expr)) {                                                \
      emitCudaError(#expr, jitErrorBuffer, status, loc);                       \
      return {};                                                               \
    }                                                                          \
  } while (false)

#define RETURN_ON_NVPTX_ERROR(x)                                               \
  do {                                                                         \
    nvPTXCompileResult result = x;                                             \
    if (result != NVPTXCOMPILE_SUCCESS) {                                      \
      emitError(loc, llvm::Twine("error: ")                                    \
                         .concat(#x)                                           \
                         .concat(" failed with error code ")                   \
                         .concat(std::to_string(result)));                     \
      return {};                                                               \
    }                                                                          \
  } while (0)

namespace {
class SerializeToCubinPass
    : public PassWrapper<SerializeToCubinPass, gpu::SerializeToBlobPass> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(SerializeToCubinPass)

  SerializeToCubinPass(StringRef triple = "nvptx64-nvidia-cuda",
                       StringRef chip = "sm_35", StringRef features = "+ptx60",
                       int llvmOptLevel = 3, int ptxasOptLevel = 3,
                       std::string ptxasPath = "",
                       std::string libDevicePath = "",
                       bool outputIntermediate = false);

  StringRef getArgument() const override { return "gpu-to-cubin"; }
  StringRef getDescription() const override {
    return "Lower GPU kernel function to CUBIN binary annotations";
  }

protected:
  LogicalResult optimizeLlvm(llvm::Module &llvmModule,
                             llvm::TargetMachine &targetMachine) override;
  std::unique_ptr<llvm::Module>
  translateToLLVMIR(llvm::LLVMContext &llvmContext) override;

private:
  void getDependentDialects(DialectRegistry &registry) const override;

  // Serializes PTX to CUBIN.
  std::unique_ptr<std::vector<char>>
  serializeISA(const std::string &isa) override;

  std::string ptxasPath;
  std::string libDevicePath;
  int llvmOptLevel;
  int ptxasOptLevel;
  bool outputIntermediate;
};
} // namespace

// Sets the 'option' to 'value' unless it already has a value.
static void maybeSetOption(Pass::Option<std::string> &option, StringRef value) {
  if (!option.hasValue())
    option = value.str();
}

SerializeToCubinPass::SerializeToCubinPass(StringRef triple, StringRef chip,
                                           StringRef features, int llvmOptLevel,
                                           int ptxasOptLevel,
                                           std::string ptxasPath,
                                           std::string libDevicePath,
                                           bool outputIntermediate) {
  maybeSetOption(this->triple, triple);
  maybeSetOption(this->chip, chip);
  maybeSetOption(this->features, features);
  this->llvmOptLevel = llvmOptLevel;
  this->ptxasOptLevel = ptxasOptLevel;
  this->ptxasPath = ptxasPath;
  this->libDevicePath = libDevicePath;
  this->outputIntermediate = outputIntermediate;
}

void SerializeToCubinPass::getDependentDialects(
    DialectRegistry &registry) const {
  registerNVVMDialectTranslation(registry);
  gpu::SerializeToBlobPass::getDependentDialects(registry);
}

std::unique_ptr<llvm::Module>
SerializeToCubinPass::translateToLLVMIR(llvm::LLVMContext &llvmContext) {

  std::unique_ptr<llvm::Module> llvmModule =
      translateModuleToLLVMIR(getOperation(), llvmContext, "LLVMDialectModule");
  if (!llvmModule)
    return llvmModule;

  if (outputIntermediate) {
    llvm::outs() << "Unoptimized GPU LLVM module for: "
                 << getOperation().getNameAttr() << "\n"
                 << *llvmModule << "\n";
    llvm::outs().flush();
  }
  LLVM_DEBUG({
    llvm::dbgs() << "Unoptimized GPU LLVM module for: "
                 << getOperation().getNameAttr() << "\n";
    llvm::dbgs() << *llvmModule << "\n";
    llvm::dbgs().flush();
  });

  // Link libdevice
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> libDevice =
      llvm::parseIRFile(libDevicePath, err, llvmContext);
  if (!libDevice || llvm::verifyModule(*libDevice, &llvm::errs())) {
    err.print("in serialize-to-cubin: Could not parse IR", llvm::errs());
    return llvmModule;
  }

  llvm::Linker::linkModules(*llvmModule, std::move(libDevice));

  // Internalize all but the public kernel function
  // (https://llvm.org/docs/NVPTXUsage.html)
  llvm::NamedMDNode *MD =
      llvmModule->getOrInsertNamedMetadata("nvvm.annotations");
  if (MD) {
    llvm::internalizeModule(
        *llvmModule, [&](const llvm::GlobalValue &GV) -> bool {
          for (auto *Op : MD->operands()) {
            llvm::MDString *KindID =
                dyn_cast<llvm::MDString>(Op->getOperand(1));
            if (!KindID || KindID->getString() == "kernel") {
              llvm::GlobalValue *KernelFn =
                  llvm::mdconst::dyn_extract_or_null<llvm::Function>(
                      Op->getOperand(0));
              if (KernelFn == &GV)
                return true;
            }
          }
          return false;
        });
  }

  SmallVector<llvm::IntrinsicInst *> toConvert;
  for (auto &F : *llvmModule) {
    for (auto &BB : F) {
      for (auto &I : BB) {
        if (auto II = dyn_cast<llvm::IntrinsicInst>(&I)) {
          toConvert.push_back(II);
        }
      }
    }
  }
  llvm::for_each(toConvert, [&](llvm::IntrinsicInst *II) {
    if (II->getIntrinsicID() == llvm::Intrinsic::powi) {
      StringRef fname;
      if (II->getArgOperand(0)->getType()->isFloatTy())
        fname = "__nv_powif";
      else if (II->getArgOperand(0)->getType()->isDoubleTy())
        fname = "__nv_powi";
      else
        assert(0 && "unhandled float type in powi call");
      auto *CI = llvm::CallInst::Create(
          II->getFunctionType(), llvmModule->getFunction(fname),
          llvm::ArrayRef<llvm::Value *>(
              {II->getArgOperand(0), II->getArgOperand(1)}),
          fname, II);
      II->replaceAllUsesWith(CI);
      II->eraseFromParent();
    }
  });

  return llvmModule;
}

LogicalResult
SerializeToCubinPass::optimizeLlvm(llvm::Module &llvmModule,
                                   llvm::TargetMachine &targetMachine) {
  if (llvmOptLevel < 0 || llvmOptLevel > 3)
    return getOperation().emitError()
           << "Invalid serizalize to gpu blob optimization level"
           << llvmOptLevel << "\n";

  targetMachine.setOptLevel(static_cast<llvm::CodeGenOpt::Level>(llvmOptLevel));

  auto transformer =
      makeOptimizingTransformer(llvmOptLevel, /*sizeLevel=*/0, &targetMachine);
  auto error = transformer(&llvmModule);
  if (error) {
    InFlightDiagnostic mlirError = getOperation()->emitError();
    llvm::handleAllErrors(
        std::move(error), [&mlirError](const llvm::ErrorInfoBase &ei) {
          mlirError << "Could not optimize LLVM IR: " << ei.message() << "\n";
        });
    return mlirError;
  }

  for (auto &F : llvmModule) {
    for (auto &BB : F) {
      for (auto &I : BB) {
        if (auto g = dyn_cast<llvm::GetElementPtrInst>(&I))
          g->setIsInBounds(true);
        if (isa<llvm::FPMathOperator>(&I))
          I.setHasAllowContract(true);
      }
    }
  }

  StripDebugInfo(llvmModule);

  if (outputIntermediate) {
    llvm::outs() << "Optimized GPU LLVM module for: "
                 << getOperation().getNameAttr() << "\n"
                 << llvmModule << "\n";
    llvm::outs().flush();
  }
  LLVM_DEBUG({
    llvm::dbgs() << "Optimized GPU LLVM module for: "
                 << getOperation().getNameAttr() << "\n";
    llvm::dbgs() << llvmModule << "\n";
    llvm::dbgs().flush();
  });

  return success();
}

std::unique_ptr<std::vector<char>>
SerializeToCubinPass::serializeISA(const std::string &isa) {
  Location loc = getOperation().getLoc();

  if (outputIntermediate) {
    llvm::outs() << "PTX module for: " << getOperation().getNameAttr() << "\n"
                 << isa << "\n";
    llvm::outs().flush();
  }
  LLVM_DEBUG({
    llvm::dbgs() << "PTX module for: " << getOperation().getNameAttr() << "\n";
    llvm::dbgs() << isa << "\n";
    llvm::dbgs().flush();
  });

  auto newTmpFile =
      [&](const char *name) -> llvm::Expected<llvm::sys::fs::TempFile> {
    auto tmpFile = llvm::sys::fs::TempFile::create(name);
    if (!tmpFile) {
      llvm::errs() << "Failed to create temp file\n";
      return tmpFile;
    }
    return tmpFile;
  };
  auto discardFile = [&](llvm::Expected<llvm::sys::fs::TempFile> &tmpFile) {
    if (!!tmpFile && tmpFile->discard()) {
      llvm::errs() << "Failed to erase temp file\n";
    }
  };
  auto tmpInput = newTmpFile("/tmp/isainput%%%%%%%.s");
  auto tmpOutput = newTmpFile("/tmp/cubinoutput%%%%%%%.cubin");
  if (!tmpInput || !tmpOutput) {
    discardFile(tmpInput);
    discardFile(tmpOutput);
    return {};
  }
  {
    llvm::raw_fd_ostream out(tmpInput->FD, /*shouldClose*/ false);
    out << isa << "\n";
    out.flush();
  }

  std::vector<StringRef> Argv;
  Argv.push_back(ptxasPath);
  Argv.push_back(llvm::Triple(triple).isArch64Bit() ? "-m64" : "-m32");
  Argv.push_back("--gpu-name");
  Argv.push_back(chip.c_str());
  Argv.push_back("--opt-level");
  Argv.push_back(std::to_string(ptxasOptLevel));
  Argv.push_back("--verbose");
  Argv.push_back("--output-file");
  Argv.push_back(tmpOutput->TmpName);
  Argv.push_back(tmpInput->TmpName);

  llvm::sys::ExecuteAndWait(ptxasPath.c_str(), Argv);

  auto MB =
      llvm::MemoryBuffer::getFile(tmpOutput->TmpName, false, false, false);
  if (MB.getError()) {
    llvm::errs() << loc << "MemoryBuffer getFile failed";
    return {};
  }
  auto membuf = std::move(*MB);

  discardFile(tmpOutput);
  discardFile(tmpInput);

  size_t cubinSize = membuf->getBufferSize();
  auto result = std::make_unique<std::vector<char>>(cubinSize);
  memcpy(&(*result)[0], membuf->getBufferStart(), cubinSize);

  return result;
}

namespace mlir::polygeist {

// Register pass to serialize GPU kernel functions to a CUBIN binary annotation.
void registerGpuSerializeToCubinPass() {
  PassRegistration<SerializeToCubinPass> registerSerializeToCubin([] {
    // Initialize LLVM NVPTX backend.
    LLVMInitializeNVPTXTarget();
    LLVMInitializeNVPTXTargetInfo();
    LLVMInitializeNVPTXTargetMC();
    LLVMInitializeNVPTXAsmPrinter();

    return std::make_unique<SerializeToCubinPass>();
  });
}

std::unique_ptr<Pass> createGpuSerializeToCubinPass(
    StringRef triple, StringRef arch, StringRef features, int llvmOptLevel,
    int ptxasOptLevel, std::string ptxasPath, std::string libDevicePath,
    bool outputIntermediate) {
  return std::make_unique<SerializeToCubinPass>(
      triple, arch, features, llvmOptLevel, ptxasOptLevel, ptxasPath,
      libDevicePath, outputIntermediate);
}

} // namespace mlir::polygeist

#else // MLIR_GPU_TO_CUBIN_PASS_ENABLE
namespace mlir::polygeist {
void registerGpuSerializeToCubinPass() {}
} // namespace mlir::polygeist
#endif // MLIR_GPU_TO_CUBIN_PASS_ENABLE
