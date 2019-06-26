//===--- generate_pcm_main.cpp - Clang module precompiling utility --------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Precompiles a Clang module map and its headers into a .pcm file using the
// same invocation that ClangImporter would use to import them, guaranteeing
// compatibility with the Swift compiler and allowing build systems that
// propagate and/or cache these to see performance improvements by not reparsing
// transitive C/Objective-C dependencies during Swift compilation.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/Basic/LLVMInitialize.h"
#include "swift/Frontend/Frontend.h"
#include "swift/Frontend/PrintingDiagnosticConsumer.h"
#include "swift/Option/Options.h"
#include "swift/Serialization/ModuleFormat.h"
#include "swift/SIL/SILModule.h"
#include "swift/Subsystems.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Bitcode/BitstreamReader.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/TargetSelect.h"

using namespace llvm::opt;
using namespace swift;

class GeneratePCMInvocation {
private:
  std::string MainExecutablePath;
  std::string OutputFilename = "-";
  std::vector<std::string> InputFilenames;
  ClangImporterOptions ClangImporterOpts;
  llvm::Triple TargetTriple;

public:
  bool hasSingleInput() const { return InputFilenames.size() == 1; }
  const std::string &getFilenameOfFirstInput() const {
    return InputFilenames[0];
  }

  void setMainExecutablePath(const std::string &Path) {
    MainExecutablePath = Path;
  }

  const std::string &getOutputFilename() { return OutputFilename; }

  const std::vector<std::string> &getInputFilenames() { return InputFilenames; }

  const ClangImporterOptions &getClangImporterOptions() {
    return ClangImporterOpts;
  }

  llvm::Triple &getTargetTriple() { return TargetTriple; }

  int parseArgs(llvm::ArrayRef<const char *> Args, DiagnosticEngine &Diags) {
    using namespace options;

    // Parse frontend command line options using Swift's option table.
    std::unique_ptr<llvm::opt::OptTable> Table = createSwiftOptTable();
    unsigned MissingIndex;
    unsigned MissingCount;
    llvm::opt::InputArgList ParsedArgs =
      Table->ParseArgs(Args, MissingIndex, MissingCount);
    if (MissingCount) {
      Diags.diagnose(SourceLoc(), diag::error_missing_arg_value,
                     ParsedArgs.getArgString(MissingIndex), MissingCount);
      return 1;
    }

    if (const Arg *A = ParsedArgs.getLastArg(options::OPT_target))
      TargetTriple = llvm::Triple(llvm::Triple::normalize(A->getValue()));
    else
      TargetTriple = llvm::Triple(llvm::sys::getDefaultTargetTriple());

    if (ParsedArgs.hasArg(OPT_UNKNOWN)) {
      ClangImporterOpts.ExtraArgs = ParsedArgs.getAllArgValues(OPT_UNKNOWN);
    }

    ClangImporterOpts.DumpClangDiagnostics = true;

    if (ParsedArgs.getLastArg(OPT_help)) {
      std::string ExecutableName = llvm::sys::path::stem(MainExecutablePath);
      Table->PrintHelp(llvm::outs(), ExecutableName.c_str(),
                       "Swift PCM Generator", 0, 0,
                       /*ShowAllAliases*/false);
      return 1;
    }

    for (const Arg *A : ParsedArgs.filtered(OPT_INPUT)) {
      InputFilenames.push_back(A->getValue());
    }

    if (InputFilenames.empty()) {
      Diags.diagnose(SourceLoc(), diag::error_mode_requires_an_input_file);
      return 1;
    }

    if (const Arg *A = ParsedArgs.getLastArg(OPT_o)) {
      OutputFilename = A->getValue();
    }

    return 0;
  }
};

int generate_pcm_main(ArrayRef<const char *> Args, const char *Argv0,
                      void *MainAddr) {
  INITIALIZE_LLVM();

  CompilerInstance Instance;
  PrintingDiagnosticConsumer PDC;
  Instance.addDiagnosticConsumer(&PDC);

  GeneratePCMInvocation Invocation;
  std::string MainExecutablePath =
      llvm::sys::fs::getMainExecutable(Argv0, MainAddr);
  Invocation.setMainExecutablePath(MainExecutablePath);

  // Parse arguments.
  if (Invocation.parseArgs(Args, Instance.getDiags()) != 0) {
    return 1;
  }

  if (!Invocation.hasSingleInput()) {
    Instance.getDiags().diagnose(SourceLoc(),
                                 diag::error_mode_requires_one_input_file);
    return 1;
  }

  StringRef Filename = Invocation.getFilenameOfFirstInput();
  auto ErrOrBuf = llvm::MemoryBuffer::getFile(Filename);
  if (!ErrOrBuf) {
    Instance.getDiags().diagnose(
        SourceLoc(), diag::error_no_such_file_or_directory, Filename);
    return 1;
  }

  // Wrap the bitstream in a module object file. To use the ClangImporter to
  // create the module loader, we need to properly set the runtime library path.
  SearchPathOptions SearchPathOpts;
  // FIXME: This logic has been duplicated from
  //        CompilerInvocation::setMainExecutablePath. ModuleWrapInvocation
  //        should share its implementation.
  SmallString<128> RuntimeResourcePath(MainExecutablePath);
  llvm::sys::path::remove_filename(RuntimeResourcePath); // Remove /swift
  llvm::sys::path::remove_filename(RuntimeResourcePath); // Remove /bin
  llvm::sys::path::append(RuntimeResourcePath, "lib", "swift");
  SearchPathOpts.RuntimeResourcePath = RuntimeResourcePath.str();

  SourceManager SrcMgr;
  LangOptions LangOpts;
  LangOpts.Target = Invocation.getTargetTriple();
  LangOpts.EnableObjCInterop = LangOpts.Target.isOSDarwin();

  ASTContext &ASTCtx = *ASTContext::get(LangOpts, SearchPathOpts, SrcMgr,
                                        Instance.getDiags());
  registerTypeCheckerRequestFunctions(ASTCtx.evaluator);

  auto clangImporter = ClangImporter::create(
      ASTCtx, Invocation.getClangImporterOptions(), "");
  auto errorOccurred =
      clangImporter->emitPrecompiledModule(Invocation.getFilenameOfFirstInput(),
                                           Invocation.getOutputFilename());

  return errorOccurred ? 1 : 0;
}

int pcm_info_main(ArrayRef<const char *> Args, const char *Argv0,
                  void *MainAddr) {
  INITIALIZE_LLVM();

  CompilerInstance Instance;
  PrintingDiagnosticConsumer PDC;
  Instance.addDiagnosticConsumer(&PDC);

  GeneratePCMInvocation Invocation;
  std::string MainExecutablePath =
      llvm::sys::fs::getMainExecutable(Argv0, MainAddr);
  Invocation.setMainExecutablePath(MainExecutablePath);

  // Parse arguments.
  if (Invocation.parseArgs(Args, Instance.getDiags()) != 0) {
    return 1;
  }

  if (!Invocation.hasSingleInput()) {
    Instance.getDiags().diagnose(SourceLoc(),
                                 diag::error_mode_requires_one_input_file);
    return 1;
  }

  StringRef Filename = Invocation.getFilenameOfFirstInput();
  auto ErrOrBuf = llvm::MemoryBuffer::getFile(Filename);
  if (!ErrOrBuf) {
    Instance.getDiags().diagnose(
        SourceLoc(), diag::error_no_such_file_or_directory, Filename);
    return 1;
  }

  // Wrap the bitstream in a module object file. To use the ClangImporter to
  // create the module loader, we need to properly set the runtime library path.
  SearchPathOptions SearchPathOpts;
  // FIXME: This logic has been duplicated from
  //        CompilerInvocation::setMainExecutablePath. ModuleWrapInvocation
  //        should share its implementation.
  SmallString<128> RuntimeResourcePath(MainExecutablePath);
  llvm::sys::path::remove_filename(RuntimeResourcePath); // Remove /swift
  llvm::sys::path::remove_filename(RuntimeResourcePath); // Remove /bin
  llvm::sys::path::append(RuntimeResourcePath, "lib", "swift");
  SearchPathOpts.RuntimeResourcePath = RuntimeResourcePath.str();

  SourceManager SrcMgr;
  LangOptions LangOpts;
  LangOpts.Target = Invocation.getTargetTriple();
  ASTContext &ASTCtx = *ASTContext::get(LangOpts, SearchPathOpts, SrcMgr,
                                        Instance.getDiags());
  registerTypeCheckerRequestFunctions(ASTCtx.evaluator);

  auto clangImporter = ClangImporter::create(
      ASTCtx, Invocation.getClangImporterOptions(), "");
  auto errorOccurred = clangImporter->dumpPrecompiledModuleInfo(
      Invocation.getFilenameOfFirstInput());

  return errorOccurred ? 1 : 0;
}
