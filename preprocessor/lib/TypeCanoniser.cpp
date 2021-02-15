#include "TypeCanoniser.hpp"
#include "Utilities.hpp"

#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendPluginRegistry.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/AST/Decl.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/TypeVisitor.h"

#include "llvm/Support/raw_ostream.h"


using namespace clang;
using namespace ast_matchers;
using namespace llvm;

TypeCanoniserASTConsumer::TypeCanoniserASTConsumer(clang::Rewriter &r,
                                  UsedFunAndTypeCollector &usedFunsAndTypes)
                           : handler(rewriter, usedFunsAndTypes), rewriter(r) {
  
  TypeLocMatcher typedefUsingTypeLocMatcher = typeLoc(loc(qualType(
    typedefType().bind("typedefType"), // matches nodes that use a typedef
    anyOf(
      //typeLoc(loc(qualType(typedefType(hasUnqualifiedDesugaredType(arrayType(hasElementType(qualType(hasDeclaration(decl(isImplicit()))))))))))
      hasCanonicalType(recordType().bind("recordType")), 
      hasCanonicalType(enumType().bind("enumType")), 
      // checking descendants looks through pointer types
      hasCanonicalType(hasDescendant(recordType().bind("recordType"))),
      hasCanonicalType(hasDescendant(enumType().bind("enumType"))),
      anything()
    )))).bind("typedefUsingTypeLoc");
  
  //finder.addMatcher(traverse(TK_IgnoreUnlessSpelledInSource, typedefUsingDeclMatcher), &handler);
  finder.addMatcher(traverse(TK_IgnoreUnlessSpelledInSource, typedefUsingTypeLocMatcher), &handler);
}

void TypeCanoniserMatcher::run(const MatchFinder::MatchResult &Result) {
  // ASTContext is used to retrieve the source location
  ASTContext *Ctx = Result.Context;

  const TypeLoc* typedefUsingTypeLoc =
    Result.Nodes.getNodeAs<clang::TypeLoc>("typedefUsingTypeLoc"); 

  if (typedefUsingTypeLoc) {       

      const RecordType * TheRecordType = 
          Result.Nodes.getNodeAs<clang::RecordType>("recordType");
      const EnumType * TheEnumType = 
          Result.Nodes.getNodeAs<clang::EnumType>("enumType");
      
      // return immediately if this is an unused record or enum type
      if (TheRecordType && !usedFunsAndTypes.typeIsSeen(TheRecordType) ||
          TheEnumType && !usedFunsAndTypes.typeIsSeen(TheEnumType)) 
        return;

      SourceLocation B = typedefUsingTypeLoc->getBeginLoc();
      SourceLocation E = typedefUsingTypeLoc->getEndLoc();

      // T a, b; --> this generate two matches for two declarations: 
      // once for both a and b. If we have replaced T once already,
      // we need to record it so we do not replace it again for b
      if (editedLocations.insert(B).second) {  //second = T if inserted       
        const TypedefType * TheTypedefType =
          Result.Nodes.getNodeAs<clang::TypedefType>("typedefType"); 

        auto canonicalType = // this does not get rid of some qualifiers
          QualType(TheTypedefType->getCanonicalTypeUnqualified());

        TypeCanoniserVisitor typeVisitor(*Ctx);
        typeVisitor.TraverseType(canonicalType); // gets rid of all qualifiers

        bool isRecordWithKindName = true; // ignore if !TheRecordType
        std::string kindName = "";
        if (TheRecordType || TheEnumType) {
          if (TheRecordType) {
            kindName = TheRecordType->getAsTagDecl()->getKindName().str();
            isRecordWithKindName = !TheRecordType->getAsTagDecl()->getNameAsString().empty();
          }
          else {
            kindName = "enum";
            isRecordWithKindName = !TheEnumType->getAsTagDecl()->getNameAsString().empty();
          }
        }

        // this adds missing kind name if necessary, e.g. struct or union
        std::string tagName = (!isRecordWithKindName ? (kindName + " ") : "");

        std::string completeTypeSpec = (tagName + 
                                        typeVisitor.getUnqualifiedTypeName());

        rewriter.ReplaceText(
            SourceRange(B, E), completeTypeSpec);
            //( (!isRecordWithKindName ? (kindName + " ") : "") +
            //QualType(canonicalType).getUnqualifiedType().getAsString()));
      }
      // Source code locations (type spec)
      /*FullSourceLoc B = Ctx->getFullLoc(TheCStyleCastExpr->getLParenLoc());
      FullSourceLoc E = Ctx->getFullLoc(TheCStyleCastExpr->getRParenLoc());

      rewriter.ReplaceText(
          SourceRange(B, E),
          ("(" +
          (!isRecordWithKindName ? (kindName + " ") : "") +
          QualType(canonicalType).getUnqualifiedType().getAsString() + 
            " /* " + TheCStyleCastExpr->getType().getAsString() + 
            " * / )"));*/
  } else {
    llvm_unreachable("Init handler called but could not determine match!\n");
  }
}

//-----------------------------------------------------------------------------
// Traverses the types bottom-up (set in declaration of TypeCanoniserVisitor)
// and extracts the unqualified type name (but also drops any pointers)
//-----------------------------------------------------------------------------
bool TypeCanoniserVisitor::VisitType(clang::Type *typ) {
    //typ->dump();
    if (typ->isPointerType())
      numPointers++;
    else {
      if (typ->isFunctionProtoType() && !isFunctionPrototype) {
        numPointers = 0;
        isFunctionPrototype = true;
        auto funTyp = typ->getAs<FunctionProtoType>();
        functionReturnTypeName = funTyp->getReturnType().getAsString();
        auto paramTypes = funTyp->getParamTypes();
        unqualTypeName = "(";
        bool start = true;
        for (auto paramType : paramTypes) {
          unqualTypeName = unqualTypeName + 
          ((!start ? ", " : "") + paramType.getAsString());
          if (start) start = false;
        }
        unqualTypeName = unqualTypeName + ")";
        unqualType = funTyp->getReturnType();
      }
      else {
        unqualType = QualType(typ, 0);
        unqualTypeName = unqualType.getAsString();   
      }
    }
    return true;
  }

  bool TypeCanoniserVisitor::VisitConstantArrayType(
    clang::ConstantArrayType *typ) {
    unqualType = typ->getCanonicalTypeInternal();
    if(typ->getSize() == 1) { // convert T x[1] to T * x;
      numPointers++;
      unqualTypeName = typ->getElementType().getAsString();
      _convertedArrayToPtr = true;
    } else
      unqualTypeName = unqualType.getAsString();
    return true;
  }
