/*++
Copyright (c) 2022 Microsoft Corporation

Module Name:

    proof_cmds.cpp

Abstract:

    Commands for reading and checking proofs.

Author:

    Nikolaj Bjorner (nbjorner) 2022-8-26

Notes:

Proof checker for clauses created during search.
1. Clauses annotated by RUP (reverse unit propagation)
   are checked to be inferrable using reverse unit propagation
   based on previous clauses.
2. Clauses annotated by supported proof rules (proof hints)
   are checked by custom proof checkers. There is a proof checker
   for each proof rule. Main proof checkers just have a single step
   but the framework allows to compose proof rules, each inference
   is checked for correctness by a plugin. 
3. When there are no supported plugin to justify the derived
   clause, or a custom check fails, the fallback is to check that the
   derived clause is a consequence of the input clauses using SMT.
   The last approach is a bail-out and offers a weaker notion of
   self-validation. It is often (but not always) sufficient for using proof
   checking for debugging, as the root-cause for an unsound inference in z3
   does not necessarily manifest when checking the conclusion of the
   inference. An external proof checker that uses such fallbacks could
   use several solvers, or bootstrap from a solver that can generate certificates
   when z3 does not.
   



--*/

#include "util/small_object_allocator.h"
#include "ast/ast_util.h"
#include "smt/smt_solver.h"
#include "sat/sat_solver.h"
#include "sat/sat_drat.h"
#include "sat/smt/euf_proof_checker.h"
#include "cmd_context/cmd_context.h"
#include "params/solver_params.hpp"
#include <iostream>

class smt_checker {
    ast_manager& m;
    params_ref   m_params;

    // for checking proof rules (hints)
    euf::proof_checker m_checker;

    // for fallback SMT checker
    scoped_ptr<solver> m_solver;

    // for RUP
    symbol       m_rup;
    sat::solver  m_sat_solver;
    sat::drat    m_drat;
    sat::literal_vector m_units;
    sat::literal_vector m_clause;

    void add_units() {
        auto const& units = m_drat.units();
        for (unsigned i = m_units.size(); i < units.size(); ++i)
            m_units.push_back(units[i].first);
    }

public:
    smt_checker(ast_manager& m):
        m(m),
        m_checker(m),
        m_sat_solver(m_params, m.limit()), 
        m_drat(m_sat_solver) 
    {
        m_params.set_bool("drat.check_unsat", true);
        m_sat_solver.updt_params(m_params);
        m_drat.updt_config();
        m_solver = mk_smt_solver(m, m_params, symbol());
        m_rup = symbol("rup");
    }

    bool is_rup(app* proof_hint) {
        return
            proof_hint &&
            proof_hint->get_name() == m_rup;        
    }

    void mk_clause(expr_ref_vector const& clause) {
        m_clause.reset();
        for (expr* e : clause) {
            bool sign = false;
            while (m.is_not(e, e))
                sign = !sign;
            m_clause.push_back(sat::literal(e->get_id(), sign));
        }
    }

    void mk_clause(expr* e) {
        m_clause.reset();
        bool sign = false;
        while (m.is_not(e, e))
            sign = !sign;
        m_clause.push_back(sat::literal(e->get_id(), sign));
    }
    
    bool check_rup(expr_ref_vector const& clause) {
        add_units();
        mk_clause(clause);
        return m_drat.is_drup(m_clause.size(), m_clause.data(), m_units);
    }

    bool check_rup(expr* u) {
        add_units();
        mk_clause(u);
        return m_drat.is_drup(m_clause.size(), m_clause.data(), m_units);
    }

    void add_clause(expr_ref_vector const& clause) {
        mk_clause(clause);
        m_drat.add(m_clause, sat::status::input());
    }

    void check(expr_ref_vector& clause, app* proof_hint) {
        
        if (is_rup(proof_hint) && check_rup(clause)) {
            std::cout << "(verified-rup)\n";
            return;
        }

        expr_ref_vector units(m);
        if (m_checker.check(clause, proof_hint, units)) {
            bool units_are_rup = true;
            for (expr* u : units) {
                if (!check_rup(u)) {
                    std::cout << "unit " << mk_pp(u, m) << " is not rup\n";
                    units_are_rup = false;
                }
            }
            if (units_are_rup) {
                std::cout << "(verified-" << proof_hint->get_name() << ")\n";
                add_clause(clause);
                return;
            }
        }

        m_solver->push();
        for (expr* lit : clause)
            m_solver->assert_expr(m.mk_not(lit));
        lbool is_sat = m_solver->check_sat();
        if (is_sat != l_false) {
            std::cout << "did not verify: " << is_sat << " " << clause << "\n\n";
            m_solver->display(std::cout);
            if (is_sat == l_true) {
                model_ref mdl;
                m_solver->get_model(mdl);
                std::cout << *mdl << "\n";
            }                
            exit(0);
        }
        m_solver->pop(1);
        std::cout << "(verified-smt)\n";
        add_clause(clause);
    }

    void assume(expr_ref_vector const& clause) {
        add_clause(clause);
        m_solver->assert_expr(mk_or(clause));
    }

    void del(expr_ref_vector const& clause) {

    }

};

namespace sat {
    /**
     * Replay proof entierly, then walk backwards extracting reduced proof.
     */
    class proof_trim {
        cmd_context& ctx;
        ast_manager& m;
        solver s;
        literal_vector m_clause;

        vector<std::tuple<literal_vector, clause*, bool, bool>> m_trail;
        
        struct hash {
            unsigned operator()(literal_vector const& v) const {
                return string_hash((char const*)v.begin(), v.size()*sizeof(literal), 3);
            }
        };
        struct eq {
            bool operator()(literal_vector const& a, literal_vector const& b) const {
                return a == b;
            }
        };
        map<literal_vector, clause_vector, hash, eq> m_clauses;
        
        void mk_clause(expr_ref_vector const& clause) {
            m_clause.reset();
            for (expr* arg: clause)
                add_literal(arg);
            std::sort(m_clause.begin(), m_clause.end());
        }
        
        bool_var mk_var(expr* arg) {
            while (arg->get_id() >= s.num_vars())
                s.mk_var(true, true);
            return arg->get_id();
        }
        
        void add_literal(expr* arg) {
            bool sign = m.is_not(arg, arg);
            m_clause.push_back(literal(mk_var(arg), sign));
        }


        /**
           Pseudo-code from Gurfinkel, Vizel, FMCAD 2014
           Input: trail (a0,d0), ..., (an,dn) = ({},bot)
           Output: reduced trail - result 
           result = []
           C = { an }
           for i = n to 0 do
               if s.is_deleted(ai) then s.revive(ai)
               else 
                  if s.isontrail(ai) then
                     s.undotrailcore(ai,C)
                  s.delete(ai)
                  if ai in C then 
                      if ai is not initial then
                         s.savetrail()
                         s.enqueue(not ai)
                         c = s.propagate()
                         s.conflictanalysiscore(c, C)
                         s.restoretrail()
                       result += [ai]
            reverse(result)
            
            is_deleted(ai):
               clause was detached
            revive(ai):
               attach clause ai
            isontrail(ai):
                some literal on the current trail in s is justified by ai
            undotrailcore(ai, C):
                pop the trail until dependencies on ai are gone
            savetrail:
                store current trail so it can be restored
            enqueue(not ai):
                assert negations of ai at a new decision level
            conflictanalysiscore(c, C):
                ?
            restoretrail:
                restore the trail to the position before enqueue
                
                                                           
        */        
        void trim() {            
            vector<literal_vector> result, clauses;
            clauses.push_back(literal_vector());
            for (unsigned i = m_trail.size(); i-- > 0; ) {
                auto const& [cl, clp, is_add, is_initial] = m_trail[i];
                if (!is_add) {
                    revive(cl, clp);
                    continue;
                }
                prune_trail(cl, clp);
                del(cl, clp);
                if (!clauses.contains(cl))
                    continue;
                if (!is_initial) {
                    s.push();
                    unsigned lvl = s.scope_lvl();
                    for (auto lit : cl)
                        s.assign(~lit, justification(lvl));
                    s.propagate(false);
                    SASSERT(s.inconsistent());
                    conflict_analysis(clauses);
                    s.pop(1);
                }
                result.push_back(cl);
            }
            result.reverse();
        }

        void del(literal_vector const& cl, clause* cp) {
            if (cp) 
                s.detach_clause(*cp);
            else 
                del(cl);
        }

        void prune_trail(literal_vector const& cl, clause* cp) {

        }

        void conflict_analysis(vector<literal_vector> const& clauses) {
            
        }


        void revive(literal_vector const& cl, clause* cp) {
            if (cp) 
                s.attach_clause(*cp);
            else 
                s.mk_clause(cl, status::redundant());            
        }


        clause* del(literal_vector const& cl) {
            clause* cp = nullptr;
            IF_VERBOSE(3, verbose_stream() << "del: " << cl << "\n");
            if (m_clause.size() == 2) {
                s.detach_bin_clause(cl[0], cl[1], true);
                return cp;
            }
            auto* e = m_clauses.find_core(cl);            
            if (!e)
                return cp;
            auto& v = e->get_data().m_value;
            if (!v.empty()) {
                cp = v.back();
                IF_VERBOSE(3, verbose_stream() << "del: " << *cp << "\n");
                s.detach_clause(*cp);
                v.pop_back();
            }
            return cp;
        }

        void save(literal_vector const& lits, clause* cl) {
            if (!cl)                
                return;
            IF_VERBOSE(3, verbose_stream() << "add: " << *cl << "\n");
            auto& v = m_clauses.insert_if_not_there(lits, clause_vector());            
            v.push_back(cl);
        }
        
    public:
        proof_trim(cmd_context& ctx):
            ctx(ctx),
            m(ctx.m()),
            s(gparams::get_module("sat"), m.limit()) {            
        }
        
        void assume(expr_ref_vector const& _clause, bool is_initial = true) {        
            mk_clause(_clause);
            IF_VERBOSE(3, verbose_stream() << "add: " << m_clause << "\n");
            auto* cl = s.mk_clause(m_clause, status::redundant());
            m_trail.push_back({ m_clause, cl, true, is_initial });
            s.propagate(false);
            save(m_clause, cl);
        }
        
        void del(expr_ref_vector const& _clause) {
            mk_clause(_clause);
            clause* cp = del(m_clause);
            m_trail.push_back({ m_clause, cp, false, true });
        }
        
        void infer(expr_ref_vector const& _clause, app*) {
            assume(_clause, false);
        }

        void updt_params(params_ref const& p) {
            s.updt_params(p);
        }

    };
}


class proof_saver {
    cmd_context& ctx;
    ast_manager& m;
public:
    proof_saver(cmd_context& ctx):ctx(ctx), m(ctx.m()) {
        auto* s = ctx.get_solver();
        if (!s)
            ctx.set_solver_factory(mk_smt_strategic_solver_factory());
        if (!ctx.get_check_sat_result())
            ctx.set_check_sat_result(ctx.get_solver());
    }

    void assume(expr_ref_vector const& clause) {
        ctx.get_solver()->log_inference(m.mk_assumption_add(nullptr, mk_or(clause)));
    }

    void del(expr_ref_vector const& clause) {
        ctx.get_solver()->log_inference(m.mk_redundant_del(mk_or(clause)));
    }

    void infer(expr_ref_vector const& clause, app* hint) {
        ctx.get_solver()->log_inference(m.mk_lemma_add(hint, mk_or(clause)));
    }
    
};

class proof_cmds_imp : public proof_cmds {
    cmd_context&    ctx;
    ast_manager&    m;
    expr_ref_vector m_lits;
    app_ref         m_proof_hint;
    bool            m_check  = true;
    bool            m_save   = false;
    bool            m_trim   = false;
    scoped_ptr<smt_checker>     m_checker;
    scoped_ptr<proof_saver>     m_saver;
    scoped_ptr<sat::proof_trim>      m_trimmer;
    
    smt_checker& checker() { if (!m_checker) m_checker = alloc(smt_checker, m); return *m_checker; }
    proof_saver& saver() { if (!m_saver) m_saver = alloc(proof_saver, ctx); return *m_saver; }
    sat::proof_trim& trim() { if (!m_trimmer) m_trimmer = alloc(sat::proof_trim, ctx); return *m_trimmer; }
    
public:
    proof_cmds_imp(cmd_context& ctx): ctx(ctx), m(ctx.m()), m_lits(m), m_proof_hint(m) {
        updt_params(gparams::get_module("solver"));
    }

    void add_literal(expr* e) override {
        if (m.is_proof(e))
            m_proof_hint = to_app(e);
        else
            m_lits.push_back(e);
    }

    void end_assumption() override {
        if (m_check)
            checker().assume(m_lits);
        if (m_save)
            saver().assume(m_lits);
        if (m_trim)
            trim().assume(m_lits);
        m_lits.reset();
        m_proof_hint.reset();
    }

    void end_infer() override {
        if (m_check)
            checker().check(m_lits, m_proof_hint);
        if (m_save)
            saver().infer(m_lits, m_proof_hint);
        if (m_trim)
            trim().infer(m_lits, m_proof_hint);
        m_lits.reset();
        m_proof_hint.reset();
    }

    void end_deleted() override {
        if (m_check)
            checker().del(m_lits);
        if (m_save)
            saver().del(m_lits);
        if (m_trim)
            trim().del(m_lits);
        m_lits.reset();
        m_proof_hint.reset();
    }

    void updt_params(params_ref const& p) {
        solver_params sp(p);
        m_check = sp.proof_check();
        m_save  = sp.proof_save();        
        m_trim  = sp.proof_trim();
        if (m_trim)
            trim().updt_params(p);
    }
};


static proof_cmds& get(cmd_context& ctx) {
    if (!ctx.get_proof_cmds())
        ctx.set_proof_cmds(alloc(proof_cmds_imp, ctx));
    return *ctx.get_proof_cmds();
}

// assumption
class assume_cmd : public cmd {
public:
    assume_cmd():cmd("assume") {}
    char const* get_usage() const override { return "<expr>+"; }
    char const * get_descr(cmd_context& ctx) const override { return "proof command for adding assumption (input assertion)"; }
    unsigned get_arity() const override { return VAR_ARITY; }
    void prepare(cmd_context & ctx) override {}
    void finalize(cmd_context & ctx) override {}
    void failure_cleanup(cmd_context & ctx) override {}
    cmd_arg_kind next_arg_kind(cmd_context & ctx) const override { return CPK_EXPR; }    
    void set_next_arg(cmd_context & ctx, expr * arg) override { get(ctx).add_literal(arg); }
    void execute(cmd_context& ctx) override { get(ctx).end_assumption(); }
};

// deleted clause
class del_cmd : public cmd {
public:
    del_cmd():cmd("del") {}
    char const* get_usage() const override { return "<expr>+"; }
    char const * get_descr(cmd_context& ctx) const override { return "proof command for clause deletion"; }
    unsigned get_arity() const override { return VAR_ARITY; }
    void prepare(cmd_context & ctx) override {}
    void finalize(cmd_context & ctx) override {}
    void failure_cleanup(cmd_context & ctx) override {}
    cmd_arg_kind next_arg_kind(cmd_context & ctx) const override { return CPK_EXPR; }    
    void set_next_arg(cmd_context & ctx, expr * arg) override { get(ctx).add_literal(arg); }
    void execute(cmd_context& ctx) override { get(ctx).end_deleted(); }
};

// learned/redundant clause
class infer_cmd : public cmd {
public:
    infer_cmd():cmd("infer") {}
    char const* get_usage() const override { return "<expr>+"; }
    char const* get_descr(cmd_context& ctx) const override { return "proof command for learned (redundant) clauses"; }
    unsigned get_arity() const override { return VAR_ARITY; }
    void prepare(cmd_context & ctx) override {}
    void finalize(cmd_context & ctx) override {}
    void failure_cleanup(cmd_context & ctx) override {}
    cmd_arg_kind next_arg_kind(cmd_context & ctx) const override { return CPK_EXPR; }    
    void set_next_arg(cmd_context & ctx, expr * arg) override { get(ctx).add_literal(arg); }
    void execute(cmd_context& ctx) override { get(ctx).end_infer(); }
};

void install_proof_cmds(cmd_context & ctx) {
    ctx.insert(alloc(del_cmd));
    ctx.insert(alloc(infer_cmd));
    ctx.insert(alloc(assume_cmd));
}
