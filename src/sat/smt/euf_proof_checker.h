/*++
Copyright (c) 2022 Microsoft Corporation

Module Name:

    euf_proof_checker.h

Abstract:

    Plugin manager for checking EUF proofs

Author:

    Nikolaj Bjorner (nbjorner) 2022-08-25

--*/
#pragma once

#include "util/map.h"
#include "util/scoped_ptr_vector.h"
#include "ast/ast.h"

namespace euf {

    class proof_checker;

    class proof_checker_plugin {
    public:
        virtual ~proof_checker_plugin() {}
        virtual bool check(app* jst) = 0;
        virtual expr_ref_vector clause(app* jst) = 0;
        virtual void register_plugins(proof_checker& pc) = 0;
    };

    class proof_checker {
        ast_manager& m;
        scoped_ptr_vector<proof_checker_plugin> m_plugins;                          // plugins of proof checkers
        map<symbol, proof_checker_plugin*, symbol_hash_proc, symbol_eq_proc> m_map; // symbol table of proof checkers
        obj_map<expr, expr_ref_vector*> m_checked_clauses;                          // cache of previously checked proofs and their clauses.
        void add_plugin(proof_checker_plugin* p);
    public:
        proof_checker(ast_manager& m);
        ~proof_checker();
        void register_plugin(symbol const& rule, proof_checker_plugin*);
        bool check(expr* jst);
        expr_ref_vector clause(expr* jst);
        bool check(expr_ref_vector const& clause, expr* e, expr_ref_vector& units);
    };

}

