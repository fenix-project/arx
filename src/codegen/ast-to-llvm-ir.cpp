#include "codegen/ast-to-llvm-ir.h"     // for ASTToLLVMIRVisitor, compile_l...
#include <glog/logging.h>               // for COMPACT_GOOGLE_LOG_INFO, LOG
#include <llvm/ADT/iterator_range.h>    // for iterator_range
#include <llvm/ADT/SmallVector.h>       // for SmallVector
#include <llvm/ADT/StringRef.h>         // for StringRef
#include <llvm/ADT/Triple.h>            // for Triple
#include <llvm/BinaryFormat/Dwarf.h>    // for SourceLanguage, TypeKind
#include <llvm/IR/Argument.h>           // for Argument
#include <llvm/IR/BasicBlock.h>         // for BasicBlock
#include <llvm/IR/DebugInfoMetadata.h>  // for DISubprogram, DICompileUnit
#include <llvm/IR/DebugLoc.h>           // for DebugLoc
#include <llvm/IR/DIBuilder.h>          // for DIBuilder
#include <llvm/IR/Function.h>           // for Function
#include <llvm/IR/Instructions.h>       // for AllocaInst
#include <llvm/IR/IRBuilder.h>          // for IRBuilder
#include <llvm/IR/Metadata.h>           // for LLVMConstants, Metadata (ptr ...
#include <llvm/IR/Module.h>             // for Module
#include <llvm/IR/Verifier.h>           // for verifyFunction
#include <llvm/Support/Error.h>         // for ExitOnError
#include <llvm/Support/Host.h>          // for getProcessTriple
#include <llvm/Support/raw_ostream.h>   // for errs, raw_fd_ostream
#include <llvm/Support/TargetSelect.h>  // for InitializeNativeTarget, Initi...
#include <cstdio>                       // for fprintf, stderr
#include <cstdlib>                      // for exit
#include <fstream>                      // for operator<<
#include <map>                          // for map
#include <memory>                       // for unique_ptr, make_unique
#include <string>                       // for string, operator<=>
#include <utility>                      // for move
#include <vector>                       // for vector
#include "codegen/ast-to-object.h"      // for ASTToObjectVisitor
#include "jit.h"                        // for ArxJIT
#include "lexer.h"                      // for Lexer
#include "parser.h"                     // for PrototypeAST, FunctionAST

namespace llvm {
  class Value;
}

extern std::string INPUT_FILE;
extern std::string OUTPUT_FILE;
extern std::string ARX_VERSION;

auto ASTToLLVMIRVisitor::CreateFunctionType(unsigned NumArgs)
  -> llvm::DISubroutineType* {
  llvm::SmallVector<llvm::Metadata*, 8> EltTys;
  llvm::DIType* DblTy = this->getDoubleTy();

  // Add the result type.
  EltTys.push_back(DblTy);

  for (unsigned i = 0, e = NumArgs; i != e; ++i) {
    EltTys.push_back(DblTy);
  }

  return this->DBuilder->createSubroutineType(
    this->DBuilder->getOrCreateTypeArray(EltTys));
}

// DebugInfo

auto ASTToLLVMIRVisitor::getDoubleTy() -> llvm::DIType* {
  if (this->DblTy) {
    return this->DblTy;
  }

  DblTy =
    this->DBuilder->createBasicType("double", 64, llvm::dwarf::DW_ATE_float);
  return DblTy;
}

auto ASTToLLVMIRVisitor::emitLocation(ExprAST* AST) -> void {
  if (!AST) {
    return this->builder->SetCurrentDebugLocation(llvm::DebugLoc());
  }

  llvm::DIScope* Scope;
  if (this->LexicalBlocks.empty()) {
    Scope = TheCU;
  } else {
    Scope = this->LexicalBlocks.back();
  }

  this->builder->SetCurrentDebugLocation(llvm::DILocation::get(
    Scope->getContext(), AST->getLine(), AST->getCol(), Scope));
}

/**
 * @brief Code generation for FloatExprAST.
 *
 */
auto ASTToLLVMIRVisitor::visit(FloatExprAST* expr) -> void {
  this->emitLocation(expr);
  ASTToObjectVisitor::visit(expr);
}

/**
 * @brief Code generation for VariableExprAST.
 *
 */
auto ASTToLLVMIRVisitor::visit(VariableExprAST* expr) -> void {
  this->emitLocation(expr);
  ASTToObjectVisitor::visit(expr);
}

/**
 * @brief Code generation for UnaryExprAST.
 *
 */
auto ASTToLLVMIRVisitor::visit(UnaryExprAST* expr) -> void {
  this->emitLocation(expr);
  ASTToObjectVisitor::visit(expr);
}

/**
 * @brief Code generation for BinaryExprAST.
 *
 */
auto ASTToLLVMIRVisitor::visit(BinaryExprAST* expr) -> void {
  this->emitLocation(expr);
  ASTToObjectVisitor::visit(expr);
}

/**
 * @brief Code generation for CallExprAST.
 *
 */
auto ASTToLLVMIRVisitor::visit(CallExprAST* expr) -> void {
  this->emitLocation(expr);
  ASTToObjectVisitor::visit(expr);
}

/**
 * @brief Code generation for IfExprAST.
 */
auto ASTToLLVMIRVisitor::visit(IfExprAST* expr) -> void {
  this->emitLocation(expr);
  ASTToObjectVisitor::visit(expr);
}

/**
 * @brief Code generation for ForExprAST.
 *
 * @param expr A `for` expression.
 */
auto ASTToLLVMIRVisitor::visit(ForExprAST* expr) -> void {
  this->emitLocation(expr);
  ASTToObjectVisitor::visit(expr);
}

/**
 * @brief Code generation for VarExprAST.
 *
 */
auto ASTToLLVMIRVisitor::visit(VarExprAST* expr) -> void {
  this->emitLocation(expr);
  ASTToObjectVisitor::visit(expr);
}

/**
 * @brief Code generation for PrototypeExprAST.
 *
 */
auto ASTToLLVMIRVisitor::visit(PrototypeAST* expr) -> void {
  ASTToObjectVisitor::visit(expr);
}

/**
 * @brief Code generation for FunctionExprAST.
 *
 * Transfer ownership of the prototype to the function_protos map, but
 * keep a reference to it for use below.
 */
auto ASTToLLVMIRVisitor::visit(FunctionAST* expr) -> void {
  auto& P = *(expr->Proto);
  function_protos[expr->Proto->getName()] = std::move(expr->Proto);
  this->getFunction(P.getName());
  llvm::Function* TheFunction = this->result_func;

  if (!TheFunction) {
    this->result_func = nullptr;
    return;
  }

  // Create a new basic block to start insertion into.
  // std::cout << "Create a new basic block to start insertion into";
  llvm::BasicBlock* BB =
    llvm::BasicBlock::Create(*this->context, "entry", TheFunction);
  this->builder->SetInsertPoint(BB);

  /* debugging-code:start*/
  // Create a subprogram DIE for this function.
  llvm::DIFile* Unit = this->DBuilder->createFile(
    this->TheCU->getFilename(), this->TheCU->getDirectory());
  llvm::DIScope* FContext = Unit;
  unsigned LineNo = P.getLine();
  unsigned ScopeLine = LineNo;
  llvm::DISubprogram* SP = this->DBuilder->createFunction(
    FContext,
    P.getName(),
    llvm::StringRef(),
    Unit,
    LineNo,
    CreateFunctionType(TheFunction->arg_size()),
    ScopeLine,
    llvm::DINode::FlagPrototyped,
    llvm::DISubprogram::SPFlagDefinition);
  TheFunction->setSubprogram(SP);

  // Push the current scope.
  this->LexicalBlocks.push_back(SP);

  // Unset the location for the prologue emission (leading instructions with no
  // location in a function are considered part of the prologue and the
  // debugger will run past them when breaking on a function)
  this->emitLocation(nullptr);
  /* debugging-code:end*/

  // Record the function arguments in the named_values map.
  // std::cout << "Record the function arguments in the named_values map.";
  this->named_values.clear();

  unsigned ArgIdx = 0;
  for (auto& Arg : TheFunction->args()) {
    // Create an alloca for this variable.
    llvm::AllocaInst* Alloca =
      CreateEntryBlockAlloca(TheFunction, Arg.getName());

    /* debugging-code: start */
    // Create a debug descriptor for the variable.
    llvm::DILocalVariable* D = this->DBuilder->createParameterVariable(
      SP, Arg.getName(), ++ArgIdx, Unit, LineNo, this->getDoubleTy(), true);

    this->DBuilder->insertDeclare(
      Alloca,
      D,
      this->DBuilder->createExpression(),
      llvm::DILocation::get(SP->getContext(), LineNo, 0, SP),
      this->builder->GetInsertBlock());

    /* debugging-code-end */

    // Store the initial value into the alloca.
    this->builder->CreateStore(&Arg, Alloca);

    // Add arguments to variable symbol table.
    this->named_values[std::string(Arg.getName())] = Alloca;
  }

  this->emitLocation(expr->Body.get());

  expr->Body->accept(this);
  llvm::Value* RetVal = this->result_val;

  if (RetVal) {
    // Finish off the function.
    this->builder->CreateRet(RetVal);

    // Pop off the lexical block for the function.
    this->LexicalBlocks.pop_back();

    // Validate the generated code, checking for consistency.
    verifyFunction(*TheFunction);

    this->result_func = TheFunction;
    return;
  }

  // Error reading body, remove function.
  TheFunction->eraseFromParent();

  this->result_func = nullptr;

  // Pop off the lexical block for the function since we added it
  // unconditionally.
  this->LexicalBlocks.pop_back();
}

/**
 * @brief Initialize LLVM Module And PassManager.
 *
 */
auto ASTToLLVMIRVisitor::Initialize() -> void {
  ASTToObjectVisitor::Initialize();

  this->jit = this->ExitOnErr(llvm::orc::ArxJIT::Create());
  this->module->setDataLayout(this->jit->getDataLayout());

  /** Create a new builder for the module. */
  this->DBuilder = std::make_unique<llvm::DIBuilder>(*this->module);
}

/**
 * @brief Compile an AST to object file.
 *
 * @param ast The AST tree object.
 */
auto compile_llvm_ir(TreeAST* ast) -> void {
  auto codegen = new ASTToLLVMIRVisitor();

  Lexer::getNextToken();

  // Initialize the target registry etc.
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  llvm::InitializeNativeTargetAsmParser();

  codegen->Initialize();

  // Run the main "interpreter loop" now.
  LOG(INFO) << "Starting MainLoop";

  // Create the compile unit for the module.
  // Currently down as "fib.ks" as a filename since we're redirecting stdin
  // but we'd like actual source locations.
  codegen->TheCU = codegen->DBuilder->createCompileUnit(
    llvm::dwarf::DW_LANG_C,
    codegen->DBuilder->createFile("fib.ks", "."),
    "Arx Compiler",
    false,
    "",
    0);

  LOG(INFO) << "Initialize Target";

  // Add the current debug info version into the module.
  codegen->module->addModuleFlag(
    llvm::Module::Warning, "Debug Info Version", llvm::DEBUG_METADATA_VERSION);

  // Darwin only supports dwarf2.
  if (llvm::Triple(llvm::sys::getProcessTriple()).isOSDarwin()) {
    codegen->module->addModuleFlag(llvm::Module::Warning, "Dwarf Version", 2);
  }

  // Construct the DIBuilder, we do this here because we need the module.
  codegen->DBuilder = std::make_unique<llvm::DIBuilder>(*codegen->module);

  // Create the compile unit for the module.
  // Currently down as "fib.ks" as a filename since we're redirecting stdin
  // but we'd like actual source locations.
  codegen->TheCU = codegen->DBuilder->createCompileUnit(
    llvm::dwarf::DW_LANG_C,
    codegen->DBuilder->createFile("fib.arxks", "."),
    "Arx Compiler",
    false,
    "",
    0);

  // Run the main "interpreter loop" now.
  codegen->MainLoop(ast);

  // Finalize the debug info.
  codegen->DBuilder->finalize();

  // Print out all of the generated code.
  codegen->module->print(llvm::errs(), nullptr);
}

/**
 * @brief Open the Arx shell.
 *
 */
auto open_shell_llvm_ir() -> void {
  // Prime the first token.
  fprintf(stderr, "Arx %s \n", ARX_VERSION.c_str());
  fprintf(stderr, ">>> ");

  compile_llvm_ir(new TreeAST());

  exit(0);
}
