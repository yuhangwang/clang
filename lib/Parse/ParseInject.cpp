//===--- ParseInject.cpp - Reflection Parsing -----------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements parsing for C++ injection statements.
//
//===----------------------------------------------------------------------===//

#include "clang/AST/ASTConsumer.h"
#include "clang/Parse/ParseDiagnostic.h"
#include "clang/Parse/Parser.h"
#include "clang/Parse/RAIIObjectsForParser.h"
#include "clang/Sema/PrettyDeclStackTrace.h"

using namespace clang;

/// \brief Parse a C++ injection statement.
///
/// \verbatim
/// injection-statement:
///   '->' injection-capture_opt injection
///   '->' constant-expression ';'
///
/// injection-capture:
///   '[' identifier-list ']'
///
/// injection:
///   'do' compound-statement
///   class-key identifier_opt '{' member-specification_opt '}'
///   'namespace' identifier_opt '{' namespace-body '}'
///   compound-statement
///
/// \endverbatim
///
/// TODO: Implement expression injection.
StmtResult Parser::ParseCXXInjectionStmt() {
  assert(Tok.is(tok::arrow));
  SourceLocation ArrowLoc = ConsumeToken();

  // A name declared in the the injection statement is local to the injection
  // statement. Establish a new declaration scope for the following injection.
  ParseScope InjectionScope(this, Scope::DeclScope | Scope::InjectionScope);

  /*
  // Parse the optional capture list; identifiers are resolved as they are
  // parsed. We inject them later on.
  SmallVector<Decl*, 4> Captures;
  if (Tok.is(tok::l_square)) {
    BalancedDelimiterTracker T(*this, tok::l_square);
    if (T.consumeOpen()) {
      Diag(Tok, diag::err_expected) << tok::l_square;
      return StmtError();
    }

    // FIXME: Deprecate named capture. Also, note that t
    while (Tok.isNot(tok::r_square)) {
      if (Tok.isNot(tok::identifier)) {
        Diag(Tok, diag::err_expected) << tok::identifier;
        return StmtError();
      }

      IdentifierInfo* Id = Tok.getIdentifierInfo();
      SourceLocation Loc = ConsumeToken();
      DeclResult Capture = 
          Actions.ActOnInjectionCapture(IdentifierLocPair(Id, Loc));
      if (Capture.isInvalid())
        return StmtError();
      Captures.push_back(Capture.get());

      if (Tok.is(tok::comma))
        ConsumeToken();
    }

    if (T.consumeClose()) {
      Diag(Tok, diag::err_expected) << tok::r_square;
      return StmtError();
    }
  }
  */

  switch (Tok.getKind()) {
    case tok::kw_do:
      return ParseCXXBlockInjection(ArrowLoc);

    case tok::kw_struct:
    case tok::kw_class:
    case tok::kw_union:
      return ParseCXXClassInjection(ArrowLoc);

    case tok::kw_namespace:
      return ParseCXXNamespaceInjection(ArrowLoc);

    case tok::l_brace:
      llvm_unreachable("Expression capture not implemented");

    default:
      return ParseCXXReflectionInjection(ArrowLoc);
  }
}

StmtResult Parser::ParseCXXBlockInjection(SourceLocation ArrowLoc) {
  assert(Tok.is(tok::kw_do) && "expected 'do'");

  // FIXME: Save the introducer token?
  ConsumeToken();

  StmtResult Injection 
      = Actions.ActOnInjectionStmt(getCurScope(), ArrowLoc, /*IsBlock=*/true);

  // Parse the block statement as if it were a lambda '[]()stmt'. 
  ParseScope BodyScope(this, Scope::BlockScope | 
                             Scope::FnScope | 
                             Scope::DeclScope);
  Actions.ActOnStartBlockFragment(getCurScope());
  StmtResult Body = ParseCompoundStatement();
  Actions.ActOnFinishBlockFragment(getCurScope(), Body.get());
  return Injection;
}

StmtResult Parser::ParseCXXClassInjection(SourceLocation ArrowLoc) {
  assert(Tok.isOneOf(tok::kw_struct, tok::kw_class, tok::kw_union) &&
         "expected 'struct', 'class', or 'union'");

  // Pre-build the statement and associate its captures.
  StmtResult Injection 
      = Actions.ActOnInjectionStmt(getCurScope(), ArrowLoc, /*IsBlock=*/false);

  DeclSpec::TST TagType;
  if (Tok.is(tok::kw_struct))
    TagType = DeclSpec::TST_struct;
  else if (Tok.is(tok::kw_class))
    TagType = DeclSpec::TST_class;
  else
    TagType = DeclSpec::TST_union;

  SourceLocation ClassKeyLoc = ConsumeToken();
  IdentifierInfo *Id = nullptr;
  SourceLocation IdLoc;
  if (Tok.is(tok::identifier)) {
    Id = Tok.getIdentifierInfo();
    IdLoc = ConsumeToken();
  }

  // Build a tag type for the injected class.
  CXXScopeSpec SS;
  MultiTemplateParamsArg MTP;
  bool IsOwned;
  bool IsDependent;
  TypeResult TR;
  Decl *Class = Actions.ActOnTag(getCurScope(), TagType, 
                                 /*Metaclass=*/nullptr, Sema::TUK_Definition, 
                                 ClassKeyLoc, SS, Id, IdLoc, 
                                 /*AttributeList=*/nullptr, AS_none,
                                 /*ModulePrivateLoc=*/SourceLocation(), 
                                 MTP, IsOwned, IsDependent, 
                                 /*ScopedEnumKWLoc=*/SourceLocation(), 
                                 /*ScopeEnumUsesClassTag=*/false, TR,
                                 /*IsTypeSpecifier*/false);

  Actions.ActOnStartClassFragment(Injection.get(), Class);

  // Parse the class definition.
  ParsedAttributesWithRange PA(AttrFactory);
  ParseCXXMemberSpecification(ClassKeyLoc, SourceLocation(), PA, TagType,
                              Class);
  if (Class->isInvalidDecl())
    return StmtError();
  
  Actions.ActOnFinishClassFragment(Injection.get());
  return Injection;
}

StmtResult Parser::ParseCXXNamespaceInjection(SourceLocation ArrowLoc) {
  assert(Tok.is(tok::kw_namespace) && "expected 'namespace'");

  // Pre-build the statement and associate its captures.
  StmtResult Injection 
      = Actions.ActOnInjectionStmt(getCurScope(), ArrowLoc, /*IsBlock=*/false);

  SourceLocation NamespaceLoc = ConsumeToken();
  IdentifierInfo *Id = nullptr;
  SourceLocation IdLoc;
  if (Tok.is(tok::identifier)) {
    Id = Tok.getIdentifierInfo();
    IdLoc = ConsumeToken();
  } else {
    // FIXME: This shouldn't be an error. ActOnStartNamespaceDef will 
    // treat a missing identifier as the anonymous namespace, which this
    // is not. An injection into the anonymous namespace must be written
    // as:
    //
    //    -> namespace <id> { namespace { decls-to-inject } }
    //
    // Just generate a unique name for the namespace. Its guaranteed not 
    // conflict since we're in a nested scope.
    Diag(Tok.getLocation(), diag::err_expected) << tok::identifier;
    return StmtError();
  }

  BalancedDelimiterTracker T(*this, tok::l_brace);
  if (T.consumeOpen()) {
    Diag(Tok, diag::err_expected) << tok::l_brace;
    return StmtError();
  }

  ParseScope NamespaceScope(this, Scope::DeclScope);

  SourceLocation InlineLoc;
  ParsedAttributesWithRange Attrs(AttrFactory);
  UsingDirectiveDecl *ImplicitUsing = nullptr;
  Decl *Ns = Actions.ActOnStartNamespaceDef(getCurScope(), InlineLoc,
                                            NamespaceLoc, IdLoc, Id,
                                            T.getOpenLocation(), 
                                            Attrs.getList(), ImplicitUsing);

  Actions.ActOnStartNamespaceFragment(Injection.get(), Ns);

  // Parse the declarations within the namespace. Note that this will match
  // the closing brace. We don't allow nested specifiers for the vector.
  std::vector<IdentifierInfo *> NsIds;
  std::vector<SourceLocation> NsIdLocs;
  std::vector<SourceLocation> NsLocs;
  ParseInnerNamespace(NsIdLocs, NsIds, NsLocs, 0, InlineLoc, Attrs, T);

  NamespaceScope.Exit();

  Actions.ActOnFinishNamespaceDef(Ns, T.getCloseLocation());
  if (Ns->isInvalidDecl())
    return StmtError();
  
  Actions.ActOnFinishNamespaceFragment(Injection.get());
  return Injection;
}

StmtResult Parser::ParseCXXReflectionInjection(SourceLocation ArrowLoc) {
  ExprResult Reflection = ParseConstantExpression();
  if (Reflection.isInvalid())
    return StmtError();
  if (ExpectAndConsumeSemi(diag::err_expected_semi_after_expr))
    return StmtError();
  return Actions.ActOnReflectionInjection(ArrowLoc, Reflection.get());
}
