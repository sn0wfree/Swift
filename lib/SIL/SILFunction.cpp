//===--- SILFunction.cpp - Defines the SILFunction data structure ---------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/CFG.h"
#include "swift/SIL/SILModule.h"
// FIXME: For mapTypeInContext
#include "swift/AST/ArchetypeBuilder.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/GraphWriter.h"

using namespace swift;
using namespace Lowering;

SILFunction *SILFunction::create(SILModule &M, SILLinkage linkage,
                                 StringRef name,
                                 CanSILFunctionType loweredType,
                                 GenericParamList *contextGenericParams,
                                 Optional<SILLocation> loc,
                                 IsBare_t isBareSILFunction,
                                 IsTransparent_t isTrans,
                                 Inline_t isNoinline, EffectsKind E,
                                 SILFunction *insertBefore,
                                 SILDebugScope *debugScope,
                                 DeclContext *DC) {
  // Get a StringMapEntry for the function.  As a sop to error cases,
  // allow the name to have an empty string.
  llvm::StringMapEntry<SILFunction*> *entry = nullptr;
  if (!name.empty()) {
    entry = &M.FunctionTable.GetOrCreateValue(name);
    assert(!entry->getValue() && "function already exists");
    name = entry->getKey();
  }

  auto fn = new (M) SILFunction(M, linkage, name,
                                loweredType, contextGenericParams, loc,
                                isBareSILFunction, isTrans, isNoinline, E,
                                insertBefore, debugScope, DC);

  if (entry) entry->setValue(fn);
  return fn;
}

SILFunction::SILFunction(SILModule &Module, SILLinkage Linkage,
                         StringRef Name, CanSILFunctionType LoweredType,
                         GenericParamList *contextGenericParams,
                         Optional<SILLocation> Loc,
                         IsBare_t isBareSILFunction,
                         IsTransparent_t isTrans,
                         Inline_t isNoinline, EffectsKind E,
                         SILFunction *InsertBefore,
                         SILDebugScope *DebugScope,
                         DeclContext *DC)
  : Module(Module),
    Name(Name),
    LoweredType(LoweredType),
    // FIXME: Context params should be independent of the function type.
    ContextGenericParams(contextGenericParams),
    Location(Loc),
    DeclCtx(DC),
    DebugScope(DebugScope),
    Bare(isBareSILFunction),
    Transparent(isTrans),
    GlobalInitFlag(false),
    NoinlineFlag(isNoinline),
    Linkage(unsigned(Linkage)), EK(E) {
  if (InsertBefore)
    Module.functions.insert(SILModule::iterator(InsertBefore), this);
  else
    Module.functions.push_back(this);
}

SILFunction::~SILFunction() {
#ifndef NDEBUG
  // If the function is recursive, a function_ref inst inside of the function
  // will give the function a non-zero ref count triggering the assertion. Thus
  // we drop all instruction references before we erase.
  dropAllReferences();
  assert(RefCount == 0 &&
         "Function cannot be deleted while function_ref's still exist");
#endif

  getModule().FunctionTable.erase(Name);
}

void SILFunction::setDeclContext(Decl *D) {
  if (!D)
    return;
  switch (D->getKind()) {
  // These four dual-inherit from DeclContext.
  case DeclKind::Func:        DeclCtx = cast<FuncDecl>(D); break;
  case DeclKind::Constructor: DeclCtx = cast<ConstructorDecl>(D); break;
  case DeclKind::Extension:   DeclCtx = cast<ExtensionDecl>(D);   break;
  case DeclKind::Destructor:  DeclCtx = cast<DestructorDecl>(D);  break;
  default:
    DeclCtx = D->getDeclContext();
  }
  assert(DeclCtx);
}

void SILFunction::setDeclContext(Expr *E) {
  DeclCtx = dyn_cast_or_null<AbstractClosureExpr>(E);
}

ASTContext &SILFunction::getASTContext() const {
  return getModule().getASTContext();
}

Type SILFunction::mapTypeIntoContext(Type type) const {
  return ArchetypeBuilder::mapTypeIntoContext(getModule().getSwiftModule(),
                                              getContextGenericParams(),
                                              type);
}

namespace {
struct MapSILTypeIntoContext : CanTypeVisitor<MapSILTypeIntoContext, CanType> {
  const SILFunction *ContextFn;
  
  MapSILTypeIntoContext(const SILFunction *ContextFn) : ContextFn(ContextFn) {}
  
  CanType visitDependentMemberType(CanDependentMemberType t) {
    // If a dependent member type appears in lowered position, we need to lower
    // its context substitution against the associated type's abstraction
    // pattern.
    CanType astTy = ContextFn->mapTypeIntoContext(t)->getCanonicalType();
    AbstractionPattern origTy(t->getAssocType()->getArchetype());
    
    return ContextFn->getModule().Types.getLoweredType(origTy, astTy)
      .getSwiftRValueType();
  }
  
  CanType visitTupleType(CanTupleType t) {
    // Dependent members can appear in lowered position inside tuples.
    
    SmallVector<TupleTypeElt, 4> elements;
    
    for (auto &elt : t->getFields())
      elements.push_back(elt.getWithType(visit(CanType(elt.getType()))));
    
    return TupleType::get(elements, t->getASTContext())
      ->getCanonicalType();
  }
  
  CanType visitSILFunctionType(CanSILFunctionType t) {
    // Dependent members can appear in lowered position inside SIL functions.
    
    SmallVector<SILParameterInfo, 4> params;
    for (auto &param : t->getParameters())
      params.push_back(param.transform([&](CanType pt) -> CanType {
        return visit(pt);
      }));
    
    SILResultInfo result = t->getResult()
      .transform([&](CanType elt) -> CanType {
        return visit(elt);
      });
    
    return SILFunctionType::get(t->getGenericSignature(),
                                t->getExtInfo(),
                                t->getCalleeConvention(),
                                params, result,
                                t->getASTContext());
  }
  
  CanType visitType(CanType t) {
    // Other types get substituted into context normally.
    return ContextFn->mapTypeIntoContext(t)->getCanonicalType();
  }
};
} // end anonymous namespace

SILType SILFunction::mapTypeIntoContext(SILType type) const {
  CanType astTy = MapSILTypeIntoContext(this).visit(type.getSwiftRValueType());
  return SILType::getPrimitiveType(astTy, type.getCategory());
}

SILBasicBlock *SILFunction::createBasicBlock() {
  return new (getModule()) SILBasicBlock(this);
}

//===----------------------------------------------------------------------===//
//                          View CFG Implementation
//===----------------------------------------------------------------------===//

#ifndef NDEBUG

llvm::cl::opt<unsigned>
MaxColumns("view-cfg-max-columns", llvm::cl::init(80),
           llvm::cl::desc("Maximum width of a printed node"));

namespace {
enum class LongLineBehavior { None, Truncate, Wrap };
} // end anonymous namespace
llvm::cl::opt<LongLineBehavior>
LLBehavior("view-cfg-long-line-behavior",
           llvm::cl::init(LongLineBehavior::Truncate),
           llvm::cl::desc("Behavior when line width is greater than the "
                          "value provided my -view-cfg-max-columns "
                          "option"),
           llvm::cl::values(
               clEnumValN(LongLineBehavior::None, "none", "Print everything"),
               clEnumValN(LongLineBehavior::Truncate, "truncate",
                          "Truncate long lines"),
               clEnumValN(LongLineBehavior::Wrap, "wrap", "Wrap long lines"),
               clEnumValEnd));

llvm::cl::opt<bool>
RemoveUseListComments("view-cfg-remove-use-list-comments",
                      llvm::cl::init(false),
                      llvm::cl::desc("Should use list comments be removed"));

template <typename InstTy, typename CaseValueTy>
inline CaseValueTy getCaseValueForBB(const InstTy *Inst,
                                     const SILBasicBlock *BB) {
  for (unsigned i = 0, e = Inst->getNumCases(); i != e; ++i) {
    auto P = Inst->getCase(i);
    if (P.second != BB)
      continue;
    return P.first;
  }
  llvm_unreachable("Error! should never pass in BB that is not a successor");
}

namespace llvm {
template <>
struct DOTGraphTraits<SILFunction *> : public DefaultDOTGraphTraits {

  DOTGraphTraits(bool isSimple = false) : DefaultDOTGraphTraits(isSimple) {}

  static std::string getGraphName(const SILFunction *F) {
    return "CFG for '" + F->getName().str() + "' function";
  }

  static std::string getSimpleNodeLabel(const SILBasicBlock *Node,
                                        const SILFunction *F) {
    std::string OutStr;
    raw_string_ostream OSS(OutStr);
    const_cast<SILBasicBlock *>(Node)->printAsOperand(OSS, false);
    return OSS.str();
  }

  static std::string getCompleteNodeLabel(const SILBasicBlock *Node,
                                          const SILFunction *F) {
    std::string Str;
    raw_string_ostream OS(Str);

    OS << *Node;
    std::string OutStr = OS.str();
    if (OutStr[0] == '\n')
      OutStr.erase(OutStr.begin());

    // Process string output to make it nicer...
    unsigned ColNum = 0;
    unsigned LastSpace = 0;
    for (unsigned i = 0; i != OutStr.length(); ++i) {
      if (OutStr[i] == '\n') { // Left justify
        OutStr[i] = '\\';
        OutStr.insert(OutStr.begin() + i + 1, 'l');
        ColNum = 0;
        LastSpace = 0;
      } else if (RemoveUseListComments && OutStr[i] == '/' &&
                 i != (OutStr.size() - 1) && OutStr[i + 1] == '/') {
        unsigned Idx = OutStr.find('\n', i + 1); // Find end of line
        OutStr.erase(OutStr.begin() + i, OutStr.begin() + Idx);
        --i;

      } else if (ColNum == MaxColumns) { // Handle long lines.

        if (LLBehavior == LongLineBehavior::Wrap) {
          if (!LastSpace)
            LastSpace = i;
          OutStr.insert(LastSpace, "\\l...");
          ColNum = i - LastSpace;
          LastSpace = 0;
          i += 3; // The loop will advance 'i' again.
        } else if (LLBehavior == LongLineBehavior::Truncate) {
          unsigned Idx = OutStr.find('\n', i + 1); // Find end of line
          OutStr.erase(OutStr.begin() + i, OutStr.begin() + Idx);
          --i;
        }

        // Else keep trying to find a space.
      } else
        ++ColNum;
      if (OutStr[i] == ' ')
        LastSpace = i;
    }
    return OutStr;
  }

  std::string getNodeLabel(const SILBasicBlock *Node,
                           const SILFunction *Graph) {
    if (isSimple())
      return getSimpleNodeLabel(Node, Graph);
    else
      return getCompleteNodeLabel(Node, Graph);
  }

  static std::string getEdgeSourceLabel(const SILBasicBlock *Node,
                                        SILBasicBlock::const_succ_iterator I) {
    SILBasicBlock *Succ = I->getBB();
    const TermInst *Term = Node->getTerminator();

    // Label source of conditional branches with "T" or "F"
    if (auto *CBI = dyn_cast<CondBranchInst>(Term))
      return (Succ == CBI->getTrueBB()) ? "T" : "F";

    // Label source of switch edges with the associated value.
    if (auto *SI = dyn_cast<SwitchIntInst>(Term)) {
      if (SI->hasDefault() && SI->getDefaultBB() == Succ)
        return "def";

      std::string Str;
      raw_string_ostream OS(Str);

      APInt I = getCaseValueForBB<SwitchIntInst, APInt>(SI, Succ);
      OS << I;
      return OS.str();
    }

    if (auto *SEIB = dyn_cast<SwitchEnumInst>(Term)) {
      std::string Str;
      raw_string_ostream OS(Str);

      EnumElementDecl *E =
          getCaseValueForBB<SwitchEnumInst, EnumElementDecl *>(SEIB, Succ);
      OS << E->getFullName();
      return OS.str();
    }

    if (auto *SEIB = dyn_cast<SwitchEnumAddrInst>(Term)) {
      std::string Str;
      raw_string_ostream OS(Str);

      EnumElementDecl *E =
          getCaseValueForBB<SwitchEnumAddrInst, EnumElementDecl *>(SEIB, Succ);
      OS << E->getFullName();
      return OS.str();
    }

    if (auto *DMBI = dyn_cast<DynamicMethodBranchInst>(Term))
      return (Succ == DMBI->getHasMethodBB()) ? "T" : "F";

    if (auto *CCBI = dyn_cast<CheckedCastBranchInst>(Term))
      return (Succ == CCBI->getSuccessBB()) ? "T" : "F";

    if (auto *CCBI = dyn_cast<CheckedCastAddrBranchInst>(Term))
      return (Succ == CCBI->getSuccessBB()) ? "T" : "F";

    return "";
  }
};
} // end llvm namespace
#endif

void SILFunction::viewCFG() const {
/// When asserts are disabled, this should be a NoOp.
#ifndef NDEBUG
  ViewGraph(const_cast<SILFunction *>(this), "cfg" + getName().str());
#endif
}
