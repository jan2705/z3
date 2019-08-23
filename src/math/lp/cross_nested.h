/*++
  Copyright (c) 2017 Microsoft Corporation

  Module Name:

  <name>

  Abstract:

  <abstract>

  Author:
  Nikolaj Bjorner (nbjorner)
  Lev Nachmanson (levnach)

  Revision History:


  --*/
#pragma once
#include <functional>
#include "math/lp/nex.h"
namespace nla {
class cross_nested {
    struct occ {
        unsigned m_occs;
        unsigned m_power;
        occ() : m_occs(0), m_power(0) {}
        occ(unsigned k, unsigned p) : m_occs(k), m_power(p) {}
        // use the "name injection rule here"
        friend std::ostream& operator<<(std::ostream& out, const occ& c) {
            out << "(occs:" << c.m_occs <<", pow:" << c.m_power << ")";
            return out;
        }
    };

    // fields
    nex *                                 m_e;
    std::function<bool (const nex*)>      m_call_on_result;
    std::function<bool (unsigned)>        m_var_is_fixed;
    bool                                  m_done;
    std::unordered_map<lpvar, occ>        m_occurences_map;
    std::unordered_map<lpvar, unsigned>   m_powers;
    ptr_vector<nex>                       m_allocated;
    ptr_vector<nex>                       m_b_split_vec;
    int                                   m_reported;
#ifdef Z3DEBUG
    nex* m_e_clone;
#endif
    void add_to_allocated(nex* r) {
        m_allocated.push_back(r);
    }
public:
    cross_nested(std::function<bool (const nex*)> call_on_result,
                 std::function<bool (unsigned)> var_is_fixed):
        m_call_on_result(call_on_result),
        m_var_is_fixed(var_is_fixed),
        m_done(false),
        m_reported(0)
    {}

    
    void run(nex *e) {
        TRACE("nla_cn", tout << *e << "\n";);
        SASSERT(e->is_simplified());
        m_e = e;
#ifdef Z3DEBUG
        //        m_e_clone = clone(m_e);
        //        m_e_clone = normalize(m_e_clone);
#endif
        vector<nex**> front;
        explore_expr_on_front_elem(&m_e, front);
    }

    static nex** pop_front(vector<nex**>& front) {
        nex** c = front.back();
        TRACE("nla_cn", tout <<  **c << "\n";);
        front.pop_back();
        return c;
    }

    nex_sum* mk_sum() {
        auto r = new nex_sum();
        add_to_allocated(r);
        return r;
    }
    template <typename T>
    void add_children(T) { }
    
    template <typename T, typename K, typename ...Args>
    void add_children(T r, K e, Args ...  es) {
        r->add_child(e);
        add_children(r, es ...);
    }

    nex_sum* mk_sum(const ptr_vector<nex>& v) {
        auto r = new nex_sum();
        add_to_allocated(r);
        r->children() = v;
        return r;
    }

    nex_mul* mk_mul(const ptr_vector<nex>& v) {
        auto r = new nex_mul();
        add_to_allocated(r);
        r->children() = v;
        return r;
    }

    
    template <typename K, typename...Args>
    nex_sum* mk_sum(K e, Args... es) {
        auto r = new nex_sum();
        add_to_allocated(r);
        r->add_child(e);
        add_children(r, es...);
        return r;
    }
    nex_var* mk_var(lpvar j) {
        auto r = new nex_var(j);
        add_to_allocated(r);
        return r;
    }
    
    nex_mul* mk_mul() {
        auto r = new nex_mul();
        add_to_allocated(r);
        return r;
    }

    template <typename K, typename...Args>
    nex_mul* mk_mul(K e, Args... es) {
        auto r = new nex_mul();
        add_to_allocated(r);
        add_children(r, e, es...);
        return r;
    }
    
    nex_scalar* mk_scalar(const rational& v) {
        auto r = new nex_scalar(v);
        add_to_allocated(r);
        return r;
    }


    nex * mk_div(const nex* a, lpvar j) {
        TRACE("nla_cn_details", tout << "a=" << *a << ", v" << j << "\n";);
        SASSERT((a->is_mul() && a->contains(j)) || (a->is_var() && to_var(a)->var() == j));
        if (a->is_var())
            return mk_scalar(rational(1));
        ptr_vector<nex> bv;
        bool seenj = false;
        for (nex* c : to_mul(a)->children()) {
            if (!seenj) {
                if (c->contains(j)) {
                    if (!c->is_var())                     
                        bv.push_back(mk_div(c, j));
                    seenj = true;
                    continue;
                } 
            }
            bv.push_back(c);
        }
        if (bv.size() > 1) { 
            return mk_mul(bv);
        }
        if (bv.size() == 1) {
            return bv[0];
        }

        SASSERT(bv.size() == 0);
        return mk_scalar(rational(1));
    }

    nex * mk_div(const nex* a, const nex* b) {
        TRACE("nla_cn_details", tout << *a <<" / " << *b << "\n";);
        if (b->is_var()) {
            return mk_div(a, to_var(b)->var());
        }
        SASSERT(b->is_mul());
        const nex_mul *bm = to_mul(b);
        if (a->is_sum()) {
            nex_sum * r = mk_sum();
            const nex_sum * m = to_sum(a);
            for (auto e : m->children()) {
                r->add_child(mk_div(e, bm));
            }
            TRACE("nla_cn_details", tout << *r << "\n";);
            return r;
        }
        if (a->is_var() || (a->is_mul() && to_mul(a)->children().size() == 1)) {
            return mk_scalar(rational(1));
        }
        SASSERT(a->is_mul());
        const nex_mul* am = to_mul(a);
        bm->get_powers_from_mul(m_powers);
        nex_mul* ret = new nex_mul();
        for (auto e : am->children()) {
            TRACE("nla_cn_details", tout << "e=" << *e << "\n";);
            
            if (!e->is_var()) {
                SASSERT(e->is_scalar());
                ret->add_child(e);
                TRACE("nla_cn_details", tout << "continue\n";);
                continue;
            }
            SASSERT(e->is_var());
            lpvar j = to_var(e)->var();
            auto it = m_powers.find(j);
            if (it == m_powers.end()) {
                 ret->add_child(e);
            } else {
                it->second --;
                if (it->second == 0)
                    m_powers.erase(it);
            }
            TRACE("nla_cn_details", tout << *ret << "\n";);            
        }
        SASSERT(m_powers.size() == 0);
        if (ret->children().size() == 0) {
            delete ret;
            TRACE("nla_cn_details", tout << "return 1\n";);
            return mk_scalar(rational(1));
        }
        add_to_allocated(ret);
        TRACE("nla_cn_details", tout << *ret << "\n";);        
        return ret;
    }

    nex* extract_common_factor(nex* e, const vector<std::pair<lpvar, occ>> & occurences) {
        nex_sum* c = to_sum(e);
        TRACE("nla_cn", tout << "c=" << *c << "\n"; tout << "occs:"; dump_occurences(tout, occurences) << "\n";);
        unsigned size = c->children().size();
        bool have_factor = false;
        for(const auto & p : occurences) {
            if (p.second.m_occs == size) {
                have_factor = true;
                break;
            }
        }
        if (have_factor == false) return nullptr;
        nex_mul* f = mk_mul();
        for(const auto & p : occurences) { // randomize here: todo
            if (p.second.m_occs == size) {
                unsigned pow = p.second.m_power;
                while (pow --) {
                    f->add_child(mk_var(p.first));
                }
            }
        }
        return f;
    }

    static bool has_common_factor(const nex_sum* c) {
        TRACE("nla_cn", tout << "c=" << *c << "\n";);
        auto & ch = c->children();
        auto common_vars = get_vars_of_expr(ch[0]);
        for (lpvar j : common_vars) {
            bool divides_the_rest = true;
            for(unsigned i = 1; i < ch.size() && divides_the_rest; i++) {
                if (!ch[i]->contains(j))
                    divides_the_rest = false;
            }
            if (divides_the_rest) {
                TRACE("nla_cn_common_factor", tout << c << "\n";);
                return true;
            }
        }
        return false;
    }

    bool proceed_with_common_factor(nex** c, vector<nex**>& front, const vector<std::pair<lpvar, occ>> & occurences) {
        TRACE("nla_cn", tout << "c=" << **c << "\n";);
        nex* f = extract_common_factor(*c, occurences);
        if (f == nullptr) {
            TRACE("nla_cn", tout << "no common factor\n"; );
            return false;
        }
        
        nex* c_over_f = mk_div(*c, f);
        to_sum(c_over_f)->simplify(&c_over_f);
        *c = mk_mul(f, c_over_f);
        TRACE("nla_cn", tout << "common factor=" << *f << ", c=" << **c << "\ne = " << *m_e << "\n";);
        
        explore_expr_on_front_elem(&(*((*c)->children_ptr()))[1],  front);
        return true;
    }

    static void push_to_front(vector<nex**>& front, nex** e) {
        TRACE("nla_cn", tout << **e << "\n";);
        front.push_back(e);
    }
    
    static vector<nex*> copy_front(const vector<nex**>& front) {
        vector<nex*> v;
        for (nex** n: front)
            v.push_back(*n);
        return v;
    }

    static void restore_front(const vector<nex*> &copy, vector<nex**>& front) {
        SASSERT(copy.size() == front.size());
        for (unsigned i = 0; i < front.size(); i++)
            *(front[i]) = copy[i];
    }

    void pop_allocated(unsigned sz) {
        for (unsigned j = sz; j < m_allocated.size(); j ++)
            delete m_allocated[j];
        m_allocated.resize(sz);
    }
    
    void explore_expr_on_front_elem_occs(nex** c, vector<nex**>& front, const vector<std::pair<lpvar, occ>> & occurences) {
        if (proceed_with_common_factor(c, front, occurences))
            return;
        TRACE("nla_cn", tout << "save c=" << **c << "; front:"; print_front(front, tout) << "\n";);           
        nex* copy_of_c = *c;
        auto copy_of_front = copy_front(front);
        int alloc_size = m_allocated.size();
        for(auto& p : occurences) {
            SASSERT(p.second.m_occs > 1);
            lpvar j = p.first;
            if (m_var_is_fixed(j)) {
                // it does not make sense to explore fixed multupliers
                // because the interval products do not become smaller
                // after factoring those out
                continue;
            }
            explore_of_expr_on_sum_and_var(c, j, front);
            if (m_done)
                return;
            TRACE("nla_cn", tout << "before restore c=" << **c << "\nm_e=" << *m_e << "\n";);
            *c = copy_of_c;
            restore_front(copy_of_front, front);
            pop_allocated(alloc_size);
            TRACE("nla_cn", tout << "after restore c=" << **c << "\nm_e=" << *m_e << "\n";);   
        }
    }

    template <typename T>
    static std::ostream& dump_occurences(std::ostream& out, const T& occurences) {
        out << "{";
        for(const auto& p: occurences) {
            const occ& o = p.second;
            out << "(v" << p.first << "->" << o << ")";
        }
        out << "}" << std::endl;
        return out;
    }

    void explore_expr_on_front_elem(nex** c, vector<nex**>& front) {
        auto occurences = get_mult_occurences(to_sum(*c));
        TRACE("nla_cn", tout << "m_e=" << *m_e << "\nc=" << **c << ", c occurences=";
              dump_occurences(tout, occurences) << "; front:"; print_front(front, tout) << "\n";);
    
        if (occurences.empty()) {
            if(front.empty()) {
                TRACE("nla_cn", tout << "got the cn form: =" << *m_e << "\n";);
                m_done = m_call_on_result(m_e) || ++m_reported > 100;
// #ifdef Z3DEBUG
//                 nex *ce = clone(m_e);
//                 TRACE("nla_cn", tout << "ce = " << *ce <<  "\n";);
//                 nex *n = normalize(ce);
//                 TRACE("nla_cn", tout << "n = " << *n << "\nm_e_clone=" << * m_e_clone << "\n";);
//                 SASSERT(*n == *m_e_clone);
// #endif
            } else {
                nex** f = pop_front(front);
                explore_expr_on_front_elem(f, front);     
            }
        } else {
            explore_expr_on_front_elem_occs(c, front, occurences);
        }
    }
    static std::string ch(unsigned j) {
        std::stringstream s;
        s << "v" << j;
        return s.str();
        //        return (char)('a'+j);
    }

    std::ostream& print_front(const vector<nex**>& front, std::ostream& out) const {
        for (auto e : front) {
            out << **e << "\n";
        }
        return out;
    }
    // c is the sub expressiond which is going to be changed from sum to the cross nested form
    // front will be explored more
    void explore_of_expr_on_sum_and_var(nex** c, lpvar j, vector<nex**> front) {
        TRACE("nla_cn", tout << "m_e=" << *m_e << "\nc=" << **c << "\nj = " << ch(j) << "\nfront="; print_front(front, tout) << "\n";);
        if (!split_with_var(*c, j, front))
            return;
        TRACE("nla_cn", tout << "after split c=" << **c << "\nfront="; print_front(front, tout) << "\n";);
        if (front.empty()) {
            m_done = m_call_on_result(m_e) || ++m_reported > 100;
            return;
        }
        auto n = pop_front(front);
        explore_expr_on_front_elem(n, front);
    }

    void add_var_occs(lpvar j) {
        auto it = m_occurences_map.find(j);
        if (it != m_occurences_map.end()) {
            it->second.m_occs++;
            it->second.m_power = 1;
        } else {            
            m_occurences_map.insert(std::make_pair(j, occ(1, 1)));
        }
    }    

    void update_occurences_with_powers() {
        for (auto & p : m_powers) {
            lpvar j = p.first;
            unsigned jp = p.second;
            auto it = m_occurences_map.find(j);
            if (it == m_occurences_map.end()) {
                m_occurences_map[j] = occ(1, jp);
            } else {
                it->second.m_occs++;
                it->second.m_power = std::min(it->second.m_power, jp);
            }
        }
        TRACE("nla_cn_details", tout << "occs="; dump_occurences(tout, m_occurences_map) << "\n";);
    }

    
    
    void remove_singular_occurences() {
        svector<lpvar> r;
        for (const auto & p : m_occurences_map) {
            if (p.second.m_occs <= 1) {
                r.push_back(p.first);
            }
        }
        for (lpvar j : r)
            m_occurences_map.erase(j);
    }

    void clear_maps() {
        m_occurences_map.clear();
        m_powers.clear();
    }
    
    // j -> the number of expressions j appears in as a multiplier
    // The result is sorted by large number of occurences first
    vector<std::pair<lpvar, occ>> get_mult_occurences(const nex_sum* e) {
        clear_maps();
        for (const auto * ce : e->children()) {
            if (ce->is_mul()) {
                to_mul(ce)->get_powers_from_mul(m_powers);
                update_occurences_with_powers();
            } else if (ce->is_var()) {
                add_var_occs(to_var(ce)->var());
            }
        }
        remove_singular_occurences();
        TRACE("nla_cn_details", tout << "e=" << *e << "\noccs="; dump_occurences(tout, m_occurences_map) << "\n";);
        vector<std::pair<lpvar, occ>> ret;
        for (auto & p : m_occurences_map)
            ret.push_back(p);
        std::sort(ret.begin(), ret.end(), [](const std::pair<lpvar, occ>& a, const std::pair<lpvar, occ>& b) {
                                              if (a.second.m_occs > b.second.m_occs)
                                                  return true;
                                              if (a.second.m_occs < b.second.m_occs)
                                                  return false;
                                              if (a.second.m_power > b.second.m_power)
                                                  return true;
                                              if (a.second.m_power < b.second.m_power)
                                                  return false;

                                              return a.first < b.first;
                                          });
        return ret;
    }

    static bool is_divisible_by_var(nex* ce, lpvar j) {
        return (ce->is_mul() && to_mul(ce)->contains(j))
            || (ce->is_var() && to_var(ce)->var() == j);
    }
    // all factors of j go to a, the rest to b
    void pre_split(nex_sum * e, lpvar j, nex_sum*& a, nex*& b) {        
        a = mk_sum();
        m_b_split_vec.clear();
        for (nex * ce: e->children()) {
            if (is_divisible_by_var(ce, j)) {
                a->add_child(mk_div(ce , j));
            } else {
                m_b_split_vec.push_back(ce);
                TRACE("nla_cn_details", tout << "ce = " << *ce << "\n";);
                
            }        
        }
        TRACE("nla_cn_details", tout << "a = " << *a << "\n";);
        SASSERT(a->children().size() >= 2 && m_b_split_vec.size());
        nex* f;
        a->simplify(&f); 
        
        if (m_b_split_vec.size() == 1) {
            b = m_b_split_vec[0];
            TRACE("nla_cn_details", tout << "b = " << *b << "\n";);
        } else {
            SASSERT(m_b_split_vec.size() > 1);
            b = mk_sum(m_b_split_vec);
            TRACE("nla_cn_details", tout << "b = " << *b << "\n";);
        }
    }

    void update_front_with_split_with_non_empty_b(nex* &e, lpvar j, vector<nex**> & front, nex_sum* a, nex* b) {
        TRACE("nla_cn_details", tout << "b = " << *b << "\n";);
        e = mk_sum(mk_mul(mk_var(j), a), b); // e = j*a + b
        if (!a->is_linear()) {
            nex **ptr_to_a = &(to_mul(to_sum(e)->children()[0]))->children()[1];
            push_to_front(front, ptr_to_a);
        }
        
        if (b->is_sum() && !to_sum(b)->is_linear()) {
            nex **ptr_to_a = &(to_sum(e)->children()[1]);
            push_to_front(front, ptr_to_a);
        }
    }
    
   void update_front_with_split(nex* & e, lpvar j, vector<nex**> & front, nex_sum* a, nex* b) {
        if (b == nullptr) {
            e = mk_mul(mk_var(j), a);
            if (!to_sum(a)->is_linear())
                push_to_front(front, &(to_mul(e)->children()[1]));
        } else {
            update_front_with_split_with_non_empty_b(e, j, front, a, b);
        }
    }
    // it returns true if the recursion brings a cross-nested form
    bool split_with_var(nex*& e, lpvar j, vector<nex**> & front) {
        SASSERT(e->is_sum());
        TRACE("nla_cn", tout << "e = " << *e << ", j=" << ch(j) << "\n";);
        nex_sum* a; nex * b;
        pre_split(to_sum(e), j, a, b);
        /*
          When we have e without a non-trivial common factor then
          there is a variable j such that e = jP + Q, where Q has all members
          of e that do not have j as a factor, and
          P also does not have a non-trivial common factor. It is enough
          to explore only such variables to create all cross-nested forms.
        */
        
        if (has_common_factor(a)) {
            return false;
        }
        update_front_with_split(e, j, front, a, b);
        return true;
    }

    static std::unordered_set<lpvar> get_vars_of_expr(const nex *e ) {
        std::unordered_set<lpvar> r;
        switch (e->type()) {
        case expr_type::SCALAR:
            return r;
        case expr_type::SUM:
            {
                for (auto c: to_sum(e)->children())
                    for ( lpvar j : get_vars_of_expr(c))
                        r.insert(j);
            }
        case expr_type::MUL:
            {
                for (auto c: to_mul(e)->children())
                    for ( lpvar j : get_vars_of_expr(c))
                        r.insert(j);
            }
            return r;
        case expr_type::VAR:
            r.insert(to_var(e)->var());
            return r;
        default:
            TRACE("nla_cn_details", tout << e->type() << "\n";);
            SASSERT(false);
            return r;
        }
    }
    
    ~cross_nested() {
        for (auto e: m_allocated)
            delete e;
        m_allocated.clear();
    }

    bool done() const { return m_done; }
#if Z3DEBUG
    nex *clone (nex * a) {
        switch (a->type()) {
        case expr_type::VAR: {
            auto v = to_var(a);
            return mk_var(v->var());
        }
            
        case expr_type::SCALAR: {
            auto v = to_scalar(a);
            return mk_scalar(v->value());
        }
        case expr_type::MUL: {
            auto m = to_mul(a);
            auto r = mk_mul();
            for (nex * e : m->children()) {
                r->add_child(clone(e));
            }
            return r;
        }
        case expr_type::SUM: {
            auto m = to_sum(a);
            auto r = mk_sum();
            for (nex * e : m->children()) {
                r->add_child(clone(e));
            }
            return r;
        }
        default:
            SASSERT(false);
            break;
        }
        return nullptr;
    }

    nex * normalize_sum(nex_sum* a) {
        for (unsigned j = 0; j < a->size(); j ++) {
            a->children()[j] = normalize(a->children()[j]);            
        }
        nex *r;
        a->simplify(&r);
        return r;
    }

    nex * normalize_mul(nex_mul* a) {
        TRACE("nla_cn", tout << *a << "\n";);
        int sum_j = -1;
        for (unsigned j = 0; j < a->size(); j ++) {
            a->children()[j] = normalize(a->children()[j]);
            if (a->children()[j]->is_sum())
                sum_j = j;
        }

        if (sum_j == -1) {
            nex * r;
            a->simplify(&r);
            SASSERT(r->is_simplified());
            return r;
        }
        
        nex_sum *r = mk_sum();
        nex_sum *as = to_sum(a->children()[sum_j]);
        for (unsigned k = 0; k < as->size(); k++) {
            nex_mul *b = mk_mul(as->children()[k]);
            for (unsigned j = 0; j < a->size(); j ++) {
                if ((int)j != sum_j)
                    b->add_child(a->children()[j]);
            }
            nex *e;
            b->simplify(&e);
            r->add_child(e);
        }
        TRACE("nla_cn", tout << *r << "\n";);
        nex *rs = normalize_sum(r);
        SASSERT(rs->is_simplified());
        return rs;

    }

    
    
    nex * normalize(nex* a) {
        if (a->is_elementary())
            return a;
        nex *r;
        if (a->is_mul()) {
            r = normalize_mul(to_mul(a));
        } else {
            r = normalize_sum(to_sum(a));
        }
        r->sort();
        return r;
    }
#endif
    
};
}