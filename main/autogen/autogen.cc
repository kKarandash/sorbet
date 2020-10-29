#include "main/autogen/autogen.h"
#include "ast/Helpers.h"
#include "ast/ast.h"
#include "ast/treemap/treemap.h"
#include "common/formatting.h"
#include "main/autogen/autoloader.h"
#include "main/autogen/msgpack.h"

#include "CRC.h"

using namespace std;
namespace sorbet::autogen {

const Definition &DefinitionRef::data(const ParsedFile &pf) const {
    return pf.defs[_id];
}

const Reference &ReferenceRef::data(const ParsedFile &pf) const {
    return pf.refs[_id];
}

class AutogenWalk {
    vector<Definition> defs;
    vector<Reference> refs;
    vector<core::NameRef> requires;
    vector<DefinitionRef> nesting;

    enum class ScopeType { Class, Block };
    vector<ast::Send *> ignoring;
    vector<ScopeType> scopeTypes;

    UnorderedMap<void *, ReferenceRef> refMap;

    vector<core::NameRef> symbolName(core::Context ctx, core::SymbolRef sym) {
        vector<core::NameRef> out;
        while (sym.exists() && sym != core::Symbols::root()) {
            out.emplace_back(sym.data(ctx)->name);
            sym = sym.data(ctx)->owner;
        }
        reverse(out.begin(), out.end());
        return out;
    }

    vector<core::NameRef> constantName(core::Context ctx, ast::ConstantLit *cnst) {
        vector<core::NameRef> out;
        while (cnst != nullptr && cnst->original != nullptr) {
            auto &original = ast::cast_tree_nonnull<ast::UnresolvedConstantLit>(cnst->original);
            out.emplace_back(original.cnst);
            cnst = ast::cast_tree<ast::ConstantLit>(original.scope);
        }
        reverse(out.begin(), out.end());
        return out;
    }

public:
    AutogenWalk() {
        auto &def = defs.emplace_back();
        def.id = 0;
        def.type = Definition::Type::Module;
        def.defines_behavior = false;
        def.is_empty = false;
        nesting.emplace_back(def.id);
    }

    ast::TreePtr preTransformClassDef(core::Context ctx, ast::TreePtr tree) {
        auto &original = ast::cast_tree_nonnull<ast::ClassDef>(tree);

        if (!ast::isa_tree<ast::ConstantLit>(original.name)) {
            return tree;
        }
        scopeTypes.emplace_back(ScopeType::Class);

        // cerr << "preTransformClassDef(" << original->toString(ctx) << ")\n";

        auto &def = defs.emplace_back();
        def.id = defs.size() - 1;
        if (original.kind == ast::ClassDef::Kind::Class) {
            def.type = Definition::Type::Class;
        } else {
            def.type = Definition::Type::Module;
        }
        def.is_empty =
            absl::c_all_of(original.rhs, [](auto &tree) { return sorbet::ast::BehaviorHelpers::checkEmptyDeep(tree); });
        def.defines_behavior = sorbet::ast::BehaviorHelpers::checkClassDefinesBehavior(tree);

        // TODO: ref.parent_of, def.parent_ref
        // TODO: expression_range
        original.name = ast::TreeMap::apply(ctx, *this, move(original.name));
        auto it = refMap.find(original.name.get());
        ENFORCE(it != refMap.end());
        def.defining_ref = it->second;
        refs[it->second.id()].is_defining_ref = true;
        refs[it->second.id()].definitionLoc = core::Loc(ctx.file, original.loc);

        auto ait = original.ancestors.begin();
        if (original.kind == ast::ClassDef::Kind::Class && !original.ancestors.empty()) {
            // Handle the superclass at outer scope
            *ait = ast::TreeMap::apply(ctx, *this, move(*ait));
            ++ait;
        }
        // Then push a scope
        nesting.emplace_back(def.id);

        for (; ait != original.ancestors.end(); ++ait) {
            *ait = ast::TreeMap::apply(ctx, *this, move(*ait));
        }
        for (auto &ancst : original.singletonAncestors) {
            ancst = ast::TreeMap::apply(ctx, *this, move(ancst));
        }

        for (auto &ancst : original.ancestors) {
            auto *cnst = ast::cast_tree<ast::ConstantLit>(ancst);
            if (cnst == nullptr || cnst->original == nullptr) {
                // Don't include synthetic ancestors
                continue;
            }

            auto it = refMap.find(ancst.get());
            if (it == refMap.end()) {
                continue;
            }
            if (original.kind == ast::ClassDef::Kind::Class && &ancst == &original.ancestors.front()) {
                // superclass
                def.parent_ref = it->second;
            }
            refs[it->second.id()].parent_of = def.id;
        }

        return tree;
    }

    ast::TreePtr postTransformClassDef(core::Context ctx, ast::TreePtr tree) {
        auto &original = ast::cast_tree_nonnull<ast::ClassDef>(tree);

        if (!ast::isa_tree<ast::ConstantLit>(original.name)) {
            return tree;
        }

        nesting.pop_back();
        scopeTypes.pop_back();

        return tree;
    }

    ast::TreePtr preTransformBlock(core::Context ctx, ast::TreePtr block) {
        scopeTypes.emplace_back(ScopeType::Block);
        return block;
    }

    ast::TreePtr postTransformBlock(core::Context ctx, ast::TreePtr block) {
        scopeTypes.pop_back();
        return block;
    }

    bool isCBaseConstant(ast::ConstantLit *cnst) {
        while (cnst != nullptr && cnst->original != nullptr) {
            auto &original = ast::cast_tree_nonnull<ast::UnresolvedConstantLit>(cnst->original);
            cnst = ast::cast_tree<ast::ConstantLit>(original.scope);
        }
        if (cnst && cnst->symbol == core::Symbols::root()) {
            return true;
        }
        return false;
    }

    ast::TreePtr postTransformConstantLit(core::Context ctx, ast::TreePtr tree) {
        auto *original = ast::cast_tree<ast::ConstantLit>(tree);

        if (!ignoring.empty()) {
            return tree;
        }
        if (original->original == nullptr) {
            return tree;
        }

        auto &ref = refs.emplace_back();
        ref.id = refs.size() - 1;
        if (isCBaseConstant(original)) {
            ref.scope = nesting.front();
        } else {
            ref.nesting = nesting;
            reverse(ref.nesting.begin(), ref.nesting.end());
            ref.nesting.pop_back();
            ref.scope = nesting.back();
        }
        ref.loc = core::Loc(ctx.file, original->loc);

        // This will get overridden if this loc is_defining_ref at the point
        // where we set that flag.
        ref.definitionLoc = core::Loc(ctx.file, original->loc);
        ref.name = constantName(ctx, original);
        auto sym = original->symbol;
        if (!sym.data(ctx)->isClassOrModule() || sym != core::Symbols::StubModule()) {
            ref.resolved = symbolName(ctx, sym);
        }
        ref.is_resolved_statically = true;
        ref.is_defining_ref = false;
        // if we're already in the scope of the class (which will be the newest-created one) then we're looking at the
        // `ancestors` or `singletonAncestors` values. Otherwise, (at least for the parent relationships we care about)
        // we're looking at the first `class Child < Parent` relationship, so we change `is_subclassing` to true.
        if (!defs.empty() && !nesting.empty() && defs.back().id._id != nesting.back()._id) {
            ref.parentKind = ClassKind::Class;
        }
        refMap[tree.get()] = ref.id;
        return tree;
    }

    ast::TreePtr postTransformAssign(core::Context ctx, ast::TreePtr tree) {
        auto &original = ast::cast_tree_nonnull<ast::Assign>(tree);

        auto *lhs = ast::cast_tree<ast::ConstantLit>(original.lhs);
        if (lhs == nullptr || lhs->original == nullptr) {
            return tree;
        }

        auto &def = defs.emplace_back();
        def.id = defs.size() - 1;
        auto *rhs = ast::cast_tree<ast::ConstantLit>(original.rhs);
        if (rhs && rhs->symbol.exists() && !rhs->symbol.data(ctx)->isTypeAlias()) {
            def.type = Definition::Type::Alias;
            ENFORCE(refMap.count(original.rhs.get()));
            def.aliased_ref = refMap[original.rhs.get()];
        } else {
            def.type = Definition::Type::Casgn;
        }
        ENFORCE(refMap.count(original.lhs.get()));
        auto &ref = refs[refMap[original.lhs.get()].id()];
        def.defining_ref = ref.id;
        ref.is_defining_ref = true;
        ref.definitionLoc = core::Loc(ctx.file, original.loc);

        def.defines_behavior = true;
        def.is_empty = false;

        return tree;
    }

    ast::TreePtr preTransformSend(core::Context ctx, ast::TreePtr tree) {
        auto *original = ast::cast_tree<ast::Send>(tree);

        bool inBlock = !scopeTypes.empty() && scopeTypes.back() == ScopeType::Block;
        // Ignore keepForIde nodes. Also ignore include/extend sends iff they are directly at the
        // class/module level. These cases are handled in `preTransformClassDef`. Do not ignore in
        // block scope so that we a ref to the included module is still rendered.
        if (original->fun == core::Names::keepForIde() ||
            (!inBlock && original->recv->isSelfReference() &&
             (original->fun == core::Names::include() || original->fun == core::Names::extend()))) {
            ignoring.emplace_back(original);
        }
        if (original->flags.isPrivateOk && original->fun == core::Names::require() && original->args.size() == 1) {
            auto *lit = ast::cast_tree<ast::Literal>(original->args.front());
            if (lit && lit->isString(ctx)) {
                requires.emplace_back(lit->asString(ctx));
            }
        }
        return tree;
    }
    ast::TreePtr postTransformSend(core::Context ctx, ast::TreePtr tree) {
        auto *original = ast::cast_tree<ast::Send>(tree);
        if (!ignoring.empty() && ignoring.back() == original) {
            ignoring.pop_back();
        }
        return tree;
    }

    ParsedFile parsedFile() {
        ENFORCE(scopeTypes.empty());

        ParsedFile out;
        out.refs = move(refs);
        out.defs = move(defs);
        out.requires = move(requires);
        return out;
    }
};

ParsedFile Autogen::generate(core::Context ctx, ast::ParsedFile tree) {
    AutogenWalk walk;
    tree.tree = ast::TreeMap::apply(ctx, walk, move(tree.tree));
    auto pf = walk.parsedFile();
    pf.path = string(tree.file.data(ctx).path());
    auto src = tree.file.data(ctx).source();
    pf.cksum = CRC::Calculate(src.data(), src.size(), CRC::CRC_32());
    pf.tree = move(tree);
    return pf;
}

vector<core::NameRef> ParsedFile::showFullName(const core::GlobalState &gs, DefinitionRef id) const {
    auto &def = id.data(*this);
    if (!def.defining_ref.exists()) {
        return {};
    }
    auto &ref = def.defining_ref.data(*this);
    auto scope = showFullName(gs, ref.scope);
    scope.insert(scope.end(), ref.name.begin(), ref.name.end());
    return scope;
}

string ParsedFile::toString(const core::GlobalState &gs) const {
    fmt::memory_buffer out;
    auto nameToString = [&](const auto &nm) -> string { return nm.data(gs)->show(gs); };

    fmt::format_to(out,
                   "# ParsedFile: {}\n"
                   "requires: [{}]\n"
                   "## defs:\n",
                   path, fmt::map_join(requires, ", ", nameToString));

    for (auto &def : defs) {
        string_view type;
        switch (def.type) {
            case Definition::Type::Module:
                type = "module"sv;
                break;
            case Definition::Type::Class:
                type = "class"sv;
                break;
            case Definition::Type::Casgn:
                type = "casgn"sv;
                break;
            case Definition::Type::Alias:
                type = "alias"sv;
                break;
        }

        fmt::format_to(out,
                       "[def id={}]\n"
                       " type={}\n"
                       " defines_behavior={}\n"
                       " is_empty={}\n",
                       def.id.id(), type, (int)def.defines_behavior, (int)def.is_empty);

        if (def.defining_ref.exists()) {
            auto &ref = def.defining_ref.data(*this);
            fmt::format_to(out, " defining_ref=[{}]\n", fmt::map_join(ref.name, " ", nameToString));
        }
        if (def.parent_ref.exists()) {
            auto &ref = def.parent_ref.data(*this);
            fmt::format_to(out, " parent_ref=[{}]\n", fmt::map_join(ref.name, " ", nameToString));
        }
        if (def.aliased_ref.exists()) {
            auto &ref = def.aliased_ref.data(*this);
            fmt::format_to(out, " aliased_ref=[{}]\n", fmt::map_join(ref.name, " ", nameToString));
        }
    }
    fmt::format_to(out, "## refs:\n");
    for (auto &ref : refs) {
        vector<string> nestingStrings;
        for (auto &scope : ref.nesting) {
            auto fullScopeName = showFullName(gs, scope);
            nestingStrings.emplace_back(fmt::format("[{}]", fmt::map_join(fullScopeName, " ", nameToString)));
        }

        auto refFullName = showFullName(gs, ref.scope);
        fmt::format_to(out,
                       "[ref id={}]\n"
                       " scope=[{}]\n"
                       " name=[{}]\n"
                       " nesting=[{}]\n"
                       " resolved=[{}]\n"
                       " loc={}\n"
                       " is_defining_ref={}\n",

                       ref.id.id(), fmt::map_join(refFullName, " ", nameToString),
                       fmt::map_join(ref.name, " ", nameToString), fmt::join(nestingStrings, " "),
                       fmt::map_join(ref.resolved, " ", nameToString), ref.loc.filePosToString(gs),
                       (int)ref.is_defining_ref);

        if (ref.parent_of.exists()) {
            auto parentOfFullName = showFullName(gs, ref.parent_of);
            fmt::format_to(out, " parent_of=[{}]\n", fmt::map_join(parentOfFullName, " ", nameToString));
        }
    }
    return to_string(out);
}

string ParsedFile::toMsgpack(core::Context ctx, int version) {
    MsgpackWriter write(version);
    return write.pack(ctx, *this);
}

vector<string> ParsedFile::listAllClasses(core::Context ctx) {
    vector<string> out;

    for (auto &def : defs) {
        if (def.type != Definition::Type::Class) {
            continue;
        }
        vector<core::NameRef> names = showFullName(ctx, def.id);
        out.emplace_back(fmt::format("{}", fmt::map_join(names, "::", [&ctx](const core::NameRef &nm) -> string_view {
                                         return nm.data(ctx)->shortName(ctx);
                                     })));
    }

    return out;
}

} // namespace sorbet::autogen
