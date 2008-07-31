#include "PLN.h"

#ifdef WIN32
#pragma warning(disable : 4311)
#endif

#include <stdlib.h>
#include <time.h>

#include "RuleProvider.h"
#include "Rules.h"
#include "spacetime.h"
#include "AtomTableWrapper.h"
#include <CogServer.h>
#include <utils2.h>
#include "PTLEvaluator.h"
#include "BackInferenceTreeNode.h"
#include <boost/iterator/indirect_iterator.hpp>
#include <boost/foreach.hpp>

enum FitnessEvalutorT { DETERMINISTIC, RANDOM, SOFTMAX };
//#include "../common/core/NMPrinter.h"

int haxxUsedProofResources = 0;
float temperature = 0.01;

extern int tempar;

namespace test
{
    long bigcount=0;

    double custom_duration = 0.0;
    clock_t custom_start, custom_finish;
    time_t custom_start2, custom_finish2;
    double custom_duration2 = 0.0;

    const bool LOG_WITH_NODE_ID = true;

    extern int _test_count;
    extern bool debugger_control;

    extern FILE *logfile;
}


namespace haxx
{
    extern bool AllowFW_VARIABLENODESinCore;
    extern uint maxDepth;
    extern reasoning::BITNodeRoot* bitnoderoot;
    extern reasoning::iAtomTableWrapper* defaultAtomTableWrapper;

    /// \todo This data must persist even if the BITNodeRoot is deleted.
    map<Handle,vector<Handle> > inferred_from;
    map<Handle,Rule*> inferred_with;
}

namespace design
{
/*  typedef boost::make_recursive_variant<
        reasoning::BITNodeRoot*,
        reasoning::ParametrizedBITNode,
        set<tree<boost::recursive_variant_ > >
    >::type BITChild;
    tree<BITChild> newBITRoot;*/

    struct pBITNode : public tree<int>
    {
        int x;
    };

/*  typedef boost::make_recursive_variant<
        reasoning::BITNodeRoot*,
        set<reasoning::pBITNode>,
        tree<boost::recursive_variant_ >
    >::type BITChild; */

    typedef boost::make_recursive_variant<
        reasoning::BITNodeRoot*,
        //pBITNode,
        reasoning::ParametrizedBITNode,
        set<tree<boost::recursive_variant_> >
    > BITChild;

    tree<BITChild> newBITRoot;

//  boost::variant<reasoning::BITNodeRoot*, reasoning::set<ParametrizedBITNode>, BITChild> BITChild;
}

namespace reasoning
{
typedef pair<Rule*, vtree> directProductionArgs;

//FitnessEvalutorT FitnessEvaluator = SOFTMAX; //DETERMINISTIC;
FitnessEvalutorT FitnessEvaluator = DETERMINISTIC;
//FitnessEvalutorT FitnessEvaluator = RANDOM;

struct less_dpargs : public binary_function<directProductionArgs, directProductionArgs, bool>
    {
        bool operator()(const directProductionArgs& lhs, const directProductionArgs& rhs) const
        {
            if (lhs.first < rhs.first)
                return true;
            if (lhs.first > rhs.first)
                return false;
            if (less_vtree()(lhs.second, rhs.second))
                return true;
            if (less_vtree()(rhs.second, lhs.second))
                return false;

            return false;
        }
    };
}

namespace haxx
{
    using namespace reasoning;
    static map<directProductionArgs, boost::shared_ptr<set<BoundVertex> >, less_dpargs> DirectProducerCache;
}

namespace reasoning
{
/*  void stats::print() const
    {
        puts("stats::print()\n");

        for (map<BITNode*, set<Vertex> >::const_iterator    i = ITN2atom.begin();
                                                            i!= ITN2atom.end(); i++)
        {
//          printf("[%d]: ", (int)i->first);
            i->first->print(-10);
            foreach(Vertex v, i->second)
            {
                printf("%d\n", v2h(v));
//              printf("%d ", v2h(v));
//              NMPrinter(NMP_ALL,-10).print(v2h(v));
                NMPrinter(NMP_BRACKETED|NMP_TYPE_NAME |NMP_NODE_NAME|NMP_NODE_TYPE_NAME|NMP_TRUTH_VALUE|NMP_PRINT_TO_FILE, -10).print(v2h(v));
            }
            printf("\n");
        }
        puts("---\n");
    }
*/
const bool DIRECT_RESULTS_SPAWN = true;
const bool USE_GENERATOR_CACHE = false; //true; /// This cache gives 30% speed-up
static const float MIN_CONFIDENCE_FOR_RULE_APPLICATION = 0.00001f;
bool RECORD_TRAILS = true;
const bool PREVENT_LOOPS = false;

extern Btr<set<Handle> > ForAll_handles;
//map<BITNode*, set<BITNode*> > users;
static int ParametrizedBITNodes = 0;

void pr(pair<Handle, Handle> i);
void pr3(pair<Handle, Handle> i);

ParametrizedBITNode::ParametrizedBITNode(BITNode* _prover, Btr<bindingsT> _bindings)
: bindings(_bindings), prover(_prover)
{ prover->children.size(); ParametrizedBITNodes++; }

template<typename V, typename Vit, typename Tit>
void copy_vars(V& vars, Vit varsbegin, Tit bbvt_begin, Tit bbvt_end)
{
    copy_if(bbvt_begin, bbvt_end, inserter(vars, varsbegin), 
        bind(equal_to<Type>(), 
        bind(getTypeFun, bind(&_v2h, _1)),
        (Type)FW_VARIABLE_NODE));
}

static int more_count=0;

BITNode::BITNode()
: depth(0), Expanded(false), rule(NULL), my_bdrum(0.0f),
direct_results(Btr<set<BoundVertex> >(new set<BoundVertex>))
{
}

void BITNodeRoot::print_users(BITNode* b)
{
    foreach(BITNode* u, users[b])
        printf("[%ld] ", (long)u);
    printf("\n");
}

void BITNodeRoot::print_parents(BITNode* b)
{
    foreach(const parent_link<BITNode>& p, b->parents)
        printf("[%ld] ", (long)p.link);
    printf("\n");
}

BITNodeRoot::BITNodeRoot(meta _target, RuleProvider* _rp)
: InferenceNodes(0), exec_pool_sorted(false), rp(_rp), post_generalize_type(0)
{
    /// \todo There's a mystical bug which prevents me from putting users inside the class.
    /// I get some kind of malloc error even if I do nothing but declare the users map in the header!
//  users.clear();
    haxx::DirectProducerCache.clear();
    
    /// All CustomCrispUnificationRules must be re-created
    //ForAll_handles.reset();
//  RuleRepository::Instance().CreateCustomCrispUnificationRules();
    
    rule = NULL;
    root = this;
    bound_target = meta(new vtree);
    if (rp)
      rp = new DefaultVariableRuleProvider;
    assert(!rp->empty());
	cprintf(3, "rp ok\n");
    haxx::bitnoderoot = this;

    vtree::iterator target_it = _target->begin();
    post_generalize_type = inheritsType((Type)((int)v2h(*target_it)), VARIABLE_SCOPE_LINK)
                                    ? VARIABLE_SCOPE_LINK
                                    : inheritsType((Type)((int)v2h(*target_it)), FORALL_LINK)
                                        ? FORALL_LINK
                                        : 0;
    if (post_generalize_type)
    {
        /// VARIABLE_SCOPE_LINK( arg_list, actual_atom) -> select actual_atom:
        target_it = _target->begin(target_it);
        ++target_it;
    }

    raw_target = Btr<vtree>(new vtree(target_it));

    /**
        The 1st child corresponds to "root variable scoper", variable-bound clones of which
        will be spawned later on. Those clones will then be owned by this Root.
        To enable this, the 1st child MUST OWN the variables in the target atom of the root.
    */

    dummy_args.push_back(Btr<BoundVTree>(new BoundVTree(*raw_target)));

//  rawPrint(*raw_target, raw_target->begin(), -1);

    children.push_back(set<ParametrizedBITNode>());
	cprintf(3, "scoper...\n");
    BITNode* root_variable_scoper = CreateChild(0, NULL, dummy_args, Btr<BoundVTree>(new BoundVTree(make_vtree((Handle)NODE))),
            bindingsT(), NO_SIBLING_SPAWNING);
	cprintf(3, "scoper ok\n");
    set<Vertex> vars;
    copy_if(    raw_target->begin(),
                raw_target->end(),
                inserter(vars, vars.begin()),
                bind(equal_to<Type>(),
                    bind(getTypeFun, bind(&_v2h, _1)),
                        (Type)FW_VARIABLE_NODE));
    foreach(Vertex v, vars)
        varOwner[v].insert(root_variable_scoper);

    eval_results.push_back(set<VtreeProvider*>());
	cprintf(3, "Root ok\n");
}

/// The complexity here results from bindings and virtuality.
/// The bindings may convert some atoms to either virtual or real atoms.
/// In the end, all links must be virtual. But simple conversion is not enough,
/// because if the root link is real, then it must be collected as a direct
/// result first.

void BITNode::ForceTargetVirtual(spawn_mode spawning)
{
    AtomSpace *nm = CogServer::getAtomSpace();
    Handle *ph = v2h(&(*raw_target->begin()));
    
    if (ph && nm->isReal(*ph) && nm->getType(*ph) != FW_VARIABLE_NODE)
    {
        cprintf(2,"ForceTargetVirtual: Arg [%ld] (exists).\n", (long)*ph);
        
        boost::shared_ptr<set<BoundVertex> > directResult(new set<BoundVertex>);
        
        /// Though the target was conceived directly, it is under my pre-bindings!
        directResult->insert(BoundVertex(*ph, Btr<bindingsT>(new bindingsT)));

        addDirectResult(directResult, spawning);
        
        SetTarget(meta(new vtree(make_vtree(*ph))), Btr<bindingsT>(new bindingsT));
    }

    /// TODO: There's some redundancy here that should be removed...

    bound_target = ForceAllLinksVirtual(bound_target);
    raw_target = ForceAllLinksVirtual(raw_target);
}


BITNode* BITNodeRoot::CreateChild(int my_rule_arg_i, Rule* new_rule, const Rule::MPs& rule_args, 
                    BBvtree _target, const bindingsT& bindings,spawn_mode spawning)

{
    /// We ignore most of the args.
    BITNode* ret =  new BITNode(
                                this,
                                this,
                                1,
                                0,
                                _target,
                                (Rule*)NULL,
                                rule_args,
                                target_chain);

    exec_pool.push_back(ret);   
    exec_pool_sorted = false;

    assert(ret->children.size() == 1 && ret->args.size() == 1);
    children[0].insert(ParametrizedBITNode(ret, Btr<bindingsT>(new bindingsT)));

    return ret;
}

Btr<set<BoundVertex> > BITNodeRoot::evaluate(set<const BITNode*>* chain) const
{
assert(0);

Btr<set<BoundVertex> > results;

//  Btr<set<BoundVertex> > results = (*children[0].begin()).prover->evaluate(chain);

    if (post_generalize_type)
    {
        BoundVertex VarScopeLink(Generalize(results, post_generalize_type));
        results->clear();
        results->insert(VarScopeLink);

        return results;
    }
    else
    {
        cprintf(0, "Results:\n");
        const float min_confidence = 0.0001f;
        Btr<set<BoundVertex> > nontrivial_results(new set<BoundVertex>);

        foreach(const BoundVertex& new_result, *results)
            if (getTruthValue(v2h(new_result.value)).getConfidence() > min_confidence)
            {
                printTree(v2h(new_result.value),0,0);

                nontrivial_results->insert(new_result);
            }
        return nontrivial_results;
    }
}



int BITNode::TotalChildren() const
{
    int c=0;
    for (vector<set<ParametrizedBITNode> >::const_iterator i =  children.begin(); i!=children.end(); i++)
    {
        c+=i->size();
    }
    return c;
}

BITNodeRoot::~BITNodeRoot() {
  delete rp;
    foreach(BITNode* b, used_nodes) delete b;
}

BITNode::~BITNode() {
        assert(root);

        if (root == this)
			cprintf(3, "Dying root...");

        cprintf(4,"Dying... %ld => %ld\n", root->InferenceNodes, root->InferenceNodes-1);
        root->InferenceNodes--;
    }
/*

        ParametrizedBITNode pn(this, plink.bindings);

        set<ParametrizedBITNode>::iterator ps = plink.link->children[plink.parent_arg_i].find(pn);

        if (ps != plink.link->children[plink.parent_arg_i].end())
            plink.link->children[plink.parent_arg_i].erase(ps);

        int a2 = plink.link->children[plink.parent_arg_i].size();
    }

    for (vector< set<ParametrizedBITNode> >::iterator i =children.begin();
        i != children.end(); i++)
    {
        for (set<ParametrizedBITNode>::iterator j=i->begin(); j!=i->end(); j++)
            delete j->prover;
    }
    children.clear();
}
*/

void BITNode::SetTarget(meta _target, Btr<bindingsT> binds)
{
    raw_target = _target;
    bound_target = bind_vtree(*raw_target, *binds);
    counted_number_of_free_variables_in_target = number_of_free_variables_in_target();
}

void BITNode::Create()
{
    assert(children.empty());

    tlog(-1, "New BITnode was created to prove:");
    rawPrint(*bound_target, bound_target->begin(),-1);

    tlog(-1, "This new InferenceState needs %d args:\n", args.size());
    for (uint ari = 0; ari < args.size(); ari++)
        rawPrint(*args[ari],args[ari]->begin(),-1);

    target_chain.insert(*bound_target);

    if (!rule || rule->isComputable())
    {
        children.insert(children.begin(), args.size(), set<ParametrizedBITNode>());
    }
    eval_results.insert(eval_results.begin(), args.size(), set<VtreeProvider*>());
    assert(rule || children.size() == 1);
}

BITNode::BITNode( BITNodeRoot* _root,
    BITNode* _parent,
    unsigned int _depth,
    unsigned int _parent_arg_i,
    meta _target,
    Rule *_rule,
    const Rule::MPs& _args,
    const vtreeset& _target_chain,
    Btr<bindingsT> _pre_bindings,
    spawn_mode spawning,
    bool _create)
: raw_target(_target), depth(_depth), root(_root), Expanded(false),
rule(_rule), my_bdrum(0.0f), target_chain(_target_chain), args(_args)
{
    AtomSpace *nm = CogServer::getAtomSpace();
    if (_parent)
        addNewParent(parent_link<BITNode>(_parent, _parent_arg_i));

    try {         
        assert(!parents.empty() || !root);

        SetTarget(_target, _pre_bindings);

        if (inheritsType(nm->getType(v2h(*bound_target->begin())), LINK) &&
            ((nm->isReal(v2h(*bound_target->begin())) && !nm->getArity(v2h(*bound_target->begin()))) ||
            (!nm->isReal(v2h(*bound_target->begin())) && !bound_target->number_of_children(bound_target->begin())))
            ) {
            rawPrint(*bound_target, bound_target->begin(),-10);
            assert(0);
        }

        if (!root->rp)
        {
            root->rp = new DefaultVariableRuleProvider();
            tlog(3, "Default RuleProvider created.\n");
        }
        else tlog(3, "Parent passed on my RuleProvider.\n");

        my_bdrum = _parent->my_bdrum;

        direct_results = Btr<set<BoundVertex> >(new set<BoundVertex>);

        ForceTargetVirtual(spawning);

        if (_create) {
            tlog(2, "Creating...\n");
            Create();
        }
    } catch(string s) {
      printf("EXCEPTION IN BITNode::BITNode! %s\n",s.c_str());
      getc(stdin); getc(stdin); throw;
    } catch(...)  {
      printf("EXCEPTION IN BITNode::BITNode!\n");
      getc(stdin); getc(stdin); throw;
    }
    root->InferenceNodes++;
}

bool BITNode::eq(BITNode* rhs) const
{
    return eq(rhs->rule, rhs->args, rhs->GetTarget(), bindingsT());
}

template<typename T>
bool equal_indirect(const T& a, const T& b) { return *a == *b; }

bool BITNode::eq(Rule* r,  const Rule::MPs& _args, meta _target, const bindingsT& _pre_bindings) const
{
    try
    {
        bool ret=false;

        if (rule != r)
            ret = false;
        else if (rule && !rule->isComputable())
        {
            //  meta _final_target(bind_vtree(*_target, GetPreBindings()));
            meta _final_target(new vtree(*_target));
            ret = (*GetTarget() == *_final_target);
        }
        else
        {
            assert(!args.empty());

            meta _final_target(new vtree(*_target));
            ret= (*GetTarget() == *_final_target);

            ret = (args.size() == _args.size()
                && std::equal(args.begin(), args.end(), _args.begin(), &equal_indirect<meta>));
        }

        if (ret)
            tlog(3, ret?"EQ\n": "IN-EQ\n");

        return ret;
    } catch(PLNexception e) { puts(e.what()); puts("Apparently pre-bindings to eq() were inconsistent."); throw; }
}

BITNode* BITNode::HasChild(BITNode* new_child, int arg_i) const
{
    foreach(const ParametrizedBITNode& c, children[arg_i])
        if (c.prover->eq(new_child))
        {
            tlog(3,"Has this child already.\n");
            return c.prover;
        }

    return NULL;
}

BITNode* BITNode::HasChild(int arg_i, Rule* r, 
    const Rule::MPs& _args, meta _target, const bindingsT& _pre_bindings) const
{
    foreach(const ParametrizedBITNode& c, children[arg_i])
        if (c.prover->eq(r, _args, _target, _pre_bindings))
        {
            tlog(3,"Has this child already.\n");
            return c.prover;
        }

    return NULL;
}
    
void BITNode::addNewParent(parent_link<BITNode> new_parent)
{
    parents.insert(new_parent);

    root->users[this].insert(new_parent.link);
    root->users[this].insert(root->users[new_parent.link].begin(), root->users[new_parent.link].end());
}

BITNode* BITNode::FindNode(BITNode* new_child) const
{
    BITNode* ret = NULL;
tlog(0,"Finding...\n");
    for (uint i=0; i<children.size(); i++)
        if ((ret=HasChild(new_child, i)) != NULL)
            return ret;

    //foreach (const set<BITNode*>& sBIT, children)
    for (uint i=0; i<children.size();i++)
        foreach(const ParametrizedBITNode& c, children[i])
            if ((ret=c.prover->FindNode(new_child)) != NULL)
                return ret;

    return NULL;
}


BITNode* BITNode::FindNode(Rule* new_rule, meta _target,
    const Rule::MPs& rule_args, const bindingsT& _pre_bindings) const
{
    /// _target has to be ForceAllLinksVirtual()ized beforehand!

    BITNode* ret = NULL;

    for (uint i=0; i<children.size(); i++)
        if ((ret=HasChild(i, new_rule, rule_args, _target, _pre_bindings)) != NULL)
            return ret;

    //foreach (const set<BITNode*>& sBIT, children)
    for (uint i=0; i<children.size();i++)
        foreach(const ParametrizedBITNode& c, children[i])
            if ((ret=c.prover->FindNode(new_rule, _target, rule_args, _pre_bindings)) != NULL)
                return ret;

    return NULL;
}

struct bdrum_updater
{
    float val;
    bdrum_updater(float _val) : val(_val) {}
    void operator()(BITNode* b) { b->my_bdrum = val; }
};



/* Algorithm */ 


static int count111=0;

void BITNode::addDirectResult(boost::shared_ptr<set<BoundVertex> > directResult, spawn_mode spawning)
{
    AtomSpace *nm = CogServer::getAtomSpace();
// If we were to store the results, it would cause inconsistency because we no longer store the prebindings.
//  direct_results->insert(directResult->begin(), directResult->end());

    foreach(const BoundVertex& bv, *directResult)
    {
        tlog(-2, "Added direct result:\n");
        printTree(v2h(bv.value), 0, -2);
    }

    foreach(const BoundVertex& bv, *directResult)
    {
        if (bv.bindings)
        {
            // Remove un-owned bindings; they are inconsequential.
            // \todo Possibly the CrispuRule should be prevented from producing them in the 1st place.
            bindingsT temp_binds(*bv.bindings);
            bv.bindings->clear();
            foreach(hpair hp, temp_binds)
                if (STLhas(root->varOwner, hp.first))
                    bv.bindings->insert(hp);
        }
        if (!bv.bindings || bv.bindings->empty())
            direct_results->insert(bv);
    }

    bool bdrum_changed = false;
    
    foreach(BoundVertex bv, *directResult)
    {
        float confidence = nm->getTV(v2h(bv.value)).getConfidence();
        if (confidence > my_bdrum)
        {
            my_bdrum = confidence;
            bdrum_changed = true;
        }
    }
    
    if (bdrum_changed)
        ApplyDown(bdrum_updater(my_bdrum));
    
    if (spawning && DIRECT_RESULTS_SPAWN)
    {
		cprintf(1,"SPAWN...\n");

        /// Insert to pool the bound versions of all the other arguments of the parent
        foreach(const BoundVertex& bv, *directResult)
            if (bv.bindings && !bv.bindings->empty())
                root->spawn(bv.bindings);
            else //Proceed to Rule evaluation (if parent has other args already)
            {
                tlog(0, "Unbound result: notify parent...\n");
                Handle hh = v2h(bv.value);
                /*printTree(hh,0,2);
printf("\r%d", count111++);
fflush(stdout);
if (count111 == 9)
{ currentDebugLevel = 2; }*/
                NotifyParentOfResult(new VtreeProviderWrapper(bv.value));
            }
    }
    else
        tlog(0,"A no-spawning process.\n");

    root->exec_pool_sorted = false; 
}

bool BITNode::inferenceLoopWith(meta req)
{
    if (this == root)
        return false;

    foreach(const vtree& prev_req, target_chain)
//      if (equalVariableStructure(prev_req, *req))  /// Would disallow for multiple consequtive deduction...
        if (prev_req == *req)
        {
            vtree* preq = const_cast<vtree*>(&prev_req);

            rawPrint(*preq, preq->begin(),3);
            tlog(3,"Loops! Equal var structure with:\n");
            rawPrint(*req, req->begin(),3);
            return true;
        }

    return false;
}   
    
bool BITNode::inferenceLoop(Rule::MPs reqs)
{
    foreach(meta req, reqs)
        foreach(const vtree& prev_req, target_chain)
        if (equalVariableStructure(prev_req, *req))
        {
            rawPrint(*req, req->begin(),3);
            return true;
        }

        return false;
}

// Filter

bool BITNode::ObeysSubtreePolicy(Rule *new_rule, meta _target)
{
//  return !(inheritsType(nm->getTypeV(*_target), NODE)
//      && new_rule->computable());

    return true;
}

// Filter

bool BITNode::ObeysPoolPolicy(Rule *new_rule, meta _target)
{
    AtomSpace *nm = CogServer::getAtomSpace();
    if (inheritsType(nm->getTypeV(*_target), FW_VARIABLE_NODE))
        return false;

    /// This rejects all atoms that contain a link with >1 vars directly below it.
    /// \todo Goes over far more child atoms than necessary. Should be made smarter.
    for(vtree::post_order_iterator node = _target->begin_post(); node != _target->end_post(); ++node)
    {
        if (std::count_if(_target->begin(node), _target->end(node),
            bind(equal_to<Type>(),
                bind(getTypeFun, bind(&_v2h, _1)),
                (Type)FW_VARIABLE_NODE ))
            > 1)
        {
			cprintf(-1, "Dis-obeys pool policy:\n");
			rawPrint(*_target, _target->begin(), -1);
//          getc(stdin);

            return false;
        }
    }

    return true;
}

void BITNode::FindTemplateBIT(BITNode* new_node, BITNode*& template_node, bindingsT& template_binds) const
{
    foreach(BITNode* bit, root->BITNodeTemplates)
        if (bit->rule == new_node->rule && new_node->args.size() == bit->args.size())
        {
            template_binds.clear();

            uint i=0;
            for(; i<bit->args.size(); i++)
                if (!unifiesWithVariableChangeTo(*new_node->args[i], *bit->args[i], template_binds))
                    break;

            if (i==bit->args.size())
            {
                template_node = bit;
                return;
            }
        }   
    template_node = NULL;
}

BITNode* BITNode::CreateChild(unsigned int target_i, Rule* new_rule,
    const Rule::MPs& rule_args, BBvtree _target, const bindingsT& new_bindings,
    spawn_mode spawning)
{
    AtomSpace *nm = CogServer::getAtomSpace();
    if (this->depth == haxx::maxDepth)
    {
        puts("haxx::maxDepth !!! "); /*press enter");
        getc(stdin);*/
        return NULL;
    }

    /// If any new requirement can, upon suitable substitutions,
    /// produce a requirement higher in the tree, reject it to avoid
    /// looping.

    if ((!PREVENT_LOOPS || !inferenceLoop(rule_args)))
    {
        Btr<bindingsT> bindings(new bindingsT());
        ///haxx::
        Btr<map<Vertex, Vertex> > pre_bindingsV(new map<Vertex, Vertex>);
        Btr<map<Vertex, Vertex> > new_bindingsV(toVertexMap(new_bindings.begin(), new_bindings.end()));

        if (!consistent<Vertex>(*pre_bindingsV, *new_bindingsV, pre_bindingsV->begin(), pre_bindingsV->end(), new_bindingsV->begin(), new_bindingsV->end()))
        {
            tlog(-1, "Binding INCONSISTENT. Child not created.\n");

            /// \todo Check if coming here actually is allowed by the design, or whether it's a bug.

            return NULL;
        }
        /// Bindings from the node that spawned me:
        bindings->insert(new_bindings.begin(), new_bindings.end());
        
        /// Bindings from the Rule.o2i that produced my target:
        if (_target->bindings)
            bindings->insert(_target->bindings->begin(), _target->bindings->end());
        
        BITNode* new_node = new BITNode(
                root, this, depth+1, target_i, _target, new_rule, rule_args,
                target_chain, bindings, spawning, false);

        BITNode* template_node = NULL;
        Btr<bindingsT> template_binds(new bindingsT);
        
        if (new_rule && new_rule->isComputable()) //&& new_rule->name != "CrispUnificationRule")
        {
            FindTemplateBIT(new_node, template_node, *template_binds);

            if (template_node)
            {
                tlog(-1, "Found a template [%ld]\n", (long)template_node);

                delete new_node;

                if (this == template_node || HasAncestor(template_node))
                    return NULL;

                /// If I already have this node as a child
                foreach(const parent_link<BITNode>& p, template_node->parents)
                    if (p.link == this && p.parent_arg_i == target_i)
                    {
                        tlog(-1, "I already have this node as a child");
                        foreach(const parent_link<BITNode>& myp, parents)
                            tlog(-2, "Parent: %ld\n", (long)myp.link);
                        return template_node;
                    }

                children[target_i].insert(ParametrizedBITNode(template_node, template_binds));
                template_node->addNewParent(parent_link<BITNode>(this, target_i, template_binds));

                return template_node;
            }
            else
                root->BITNodeTemplates.insert(new_node);
        }

        root->used_nodes.insert(new_node);

        new_node->Create();

        tlog(2, "Created new BIT child [%ld]\n", (long)new_node);

        if (ObeysPoolPolicy(new_rule, _target))
        {
            root->exec_pool.push_back(new_node);
            root->exec_pool_sorted = false;
        }

        children[target_i].insert(ParametrizedBITNode(new_node, Btr<bindingsT>(new bindingsT)));

        /// Identify the _new_ variables of this set
        /// And add _new_ variables to var dependency map for the new Child (-InferenceNode)

        /// Copy all vars from the args
        set<Vertex> vars;
        foreach(const BBvtree& bbvt, rule_args)
            copy_vars(vars, vars.begin(), bbvt->begin(), bbvt->end());

        /// Find all _new_ vars
        foreach(Vertex v, vars)
            if (!STLhas2(*_target, v))
            {
                root->varOwner[v].insert(new_node);
                cprintf(0,"[%ld] owns %s\n", (long)new_node, nm->getName(v2h(v)).c_str());
            }

        return new_node;
    }

    return NULL;
}

/*
spawn() is called with all the bindings that were made to produce some direct (eg. lookup) result.
bind_key_set = all the thus bound vars
mediated_bindings = the bindings after being mediated 
*/

void BITNodeRoot::spawn(Btr<bindingsT> bindings)
{
    /// Only retain the bindings relevant to the varOwner

    map<BITNode*, bindingsT> clone_binds;
    foreach(hpair raw_pair, *bindings) //for $x=>A
        foreach(BITNode* bitn, varOwner[raw_pair.first])
            clone_binds[bitn].insert(raw_pair);
    
    typedef pair<BITNode*, bindingsT> o2bT;
    foreach(const o2bT& owner2binds, clone_binds)
    {
        cprintf(-1,"spawn next[%ld]:\n", (long)owner2binds.first);

        foreach(hpair b, owner2binds.second)
            owner2binds.first->TryClone(b);
    }
}

/// If any of the bound vars are owned, we'll spawn.
/// TODO: Actually Rules should probably not even need to inform us
/// about the bound vars if they are new, in which case this check would be redundant.

bool BITNodeRoot::spawns(const bindingsT& bindings) const
{
    foreach(hpair b, bindings)
        if (STLhas(varOwner, b.first))
            return true;

    return false;
}

bool BITNode::expandRule(Rule *new_rule, int target_i, BBvtree _target, Btr<bindingsT> bindings, spawn_mode spawning)
{   
    bool ret = true;
    try
    {
        tlog(-2, "Expanding rule... %s\n", (new_rule ? new_rule->name.c_str() : "?"));      

        if (!new_rule->isComputable())          
        {
            CreateChild(target_i, new_rule, Rule::MPs(), _target, *bindings, spawning);
        }
        else
        {
            tlog(-2, "Indirect producer expanding: %s\n", new_rule->name.c_str());
            rawPrint(*_target, _target->begin(), 2);

            if (!ObeysSubtreePolicy(new_rule, _target))
            {
                tlog(3, "Our policy is to filter this kind of BIT nodes out.\n");
                return false;
            }

            /// Different argument vectors of the same rule (eg. ForAllRule) may have different bindings.

time( &test::custom_start2 );

            meta virtualized_target(bindings ? bind_vtree(*_target, *bindings) : meta(new vtree(*_target)));
            ForceAllLinksVirtual(virtualized_target);

time( &test::custom_finish2 );
test::custom_duration2 += difftime( test::custom_finish2, test::custom_start2 );

            set<Rule::MPs> target_v_set = new_rule->o2iMeta(virtualized_target);
            
test::custom_duration += (double)(test::custom_finish - test::custom_start) / CLOCKS_PER_SEC;
                        
            if (target_v_set.empty())
            {
                tlog(3,"This rule is useless.\n");
                return false;
            }
            else
            {       
                tlog(2,"Rule.o2i gave %d results\n",target_v_set.size());

                for (set<Rule::MPs>::iterator   j =target_v_set.begin();
                                            j!=target_v_set.end();
                                            j++)
                {       
                    Btr<BoundVTree> jtree = (*j->begin());
                    Btr<bindingsT> combined_binds(new bindingsT(*bindings));

                    /** If we spawn the new bindings that this Rule needs, then we will not create the new
                         unbound childnode at all. The new (bound) version of that child will be probably
                         created later.
                         New variables may have been introduced in these bindings of jtree, eg.
                         to get Imp(A, $2) we might have got arg vector of
                         Imp(A, $1) and Imp($1, $2) where $2 = And(B, $4).
                         Because the childnode will not be created here, $1 will simply disappear.
                         $2 = And(B, $4) will be submitted to spawning. The owner of $4 will then be
                         the owner of $2. We find this out by looking at the RHS of each binding that was
                         fed to the spawning process, and finding if there are un-owned variables there.
                         $4 was clearly unwoned.

                         ToDo: we could maintain a list of un-owned variables and destroy them at proper times?
                     */

                    if (spawning == ALLOW_SIBLING_SPAWNING
                        && jtree->bindings && root->spawns(*jtree->bindings))
                        root->spawn(jtree->bindings);
                    else
                    {
                        /// haxx::
                        /// The pre-bindings of a parent node must include the pre-bindings of Rule argument nodes.
                        /// Because all the arg nodes have the same bindings, we just grab the ones from the 1st arg
                        /// and insert them to this (new) parent node.

                        if (jtree->bindings)
                            try
                            {
                                insert_with_consistency_check(*combined_binds, jtree->bindings->begin(), jtree->bindings->end());
                            } catch(...) { puts("exception in expandRule (bindings combination)"); continue; }

                        BITNode* new_node = CreateChild(target_i, new_rule, *j,_target, *combined_binds, spawning);                 
                    }
                }
            
                return !children[target_i].empty();
            }
        }
    } catch(...) { tlog(0,"Exception in ExpandRule()"); throw; }

      return ret;
}

void BITNode::TryClone(hpair binding) const
{
    foreach(const parent_link<BITNode>& p, parents)
    {
tlog(-2, "TryClone next...\n");

/// when 'binding' is from ($A to $B), try to find parent with bindings ($C to $A)

        map<Handle, Handle>::const_iterator it = find_if(p.bindings->begin(), p.bindings->end(),
                bind(equal_to<Handle>(),
                    bind(&second<Handle,Handle>, _1),
                    binding.first));

        if (p.bindings->end() != it)
        {
            p.link->TryClone(hpair(it->first, binding.second));
        }
        else
        {
            /// Create Child with this new binding:

            Rule::MPs new_args;
            Rule::CloneArgs(this->args, new_args);

            if (p.link && p.link->rule && !p.link->rule->validate2(new_args))
                continue;

            bindingsT single_bind;
            single_bind.insert(binding);

            /// Bind the args
            foreach(BBvtree& bbvt, new_args)
            {
                bbvt = BBvtree(new BoundVTree(*bind_vtree(*bbvt, single_bind)));
                ForceAllLinksVirtual(bbvt);
            }
            Btr<BoundVTree> new_target(new BoundVTree(*bind_vtree(*this->raw_target, single_bind)));

            BITNode* new_node = p.link->CreateChild(
                p.parent_arg_i,
                this->rule,
                new_args,
                new_target,
                single_bind,
                NO_SIBLING_SPAWNING); //Last arg probably redundant now
        }
    }
}

bool BITNode::HasAncestor(const BITNode* const _p) const
{
    return this != root && STLhas(root->users[(BITNode*)this], (BITNode*)_p);
}

const set<VtreeProvider*>& BITNodeRoot::infer(int& resources, float minConfidenceForStorage, float minConfidenceForAbort)
{
    AtomSpace *nm = CogServer::getAtomSpace();

    if (currentDebugLevel >= 4)
    {
        puts("Finally proving:");
        NMPrinter(NMP_ALL)(*raw_target, 4);
        getc(stdin);
    }

    //printf("printf variableScoper... %d %d\n", resources, currentDebugLevel);
    if (rule)
        puts("non-null rule");
    
    tlog(0, "variableScoper... %d\n", resources);
    tlog(0, "children %d\n", children.size());
    /// \todo Support minConfidenceForStorage

    BITNode* variableScoper = children[0].begin()->prover;

    cprintf(0,"children %ud\n", (unsigned int) children.size());

    // These are not supposed to propagate higher than variableScoper
    //assert(eval_results.empty());
    while(resources)
    {
        tlog(0, "Resources left %d\n", resources);

        resources--;
        expandFittest();

        tlog(0, "expandFittest() ok!\n");

        const vector<set<VtreeProvider*> >& eval_res_vector_set = eval_results;
        
        foreach(VtreeProvider* vtp, *eval_res_vector_set.begin())
        {
            tlog(0, "get next TV\n");
            //assert(vt2h(*vtp)->isReal());
            assert(nm->isReal(vt2h(*vtp)));
            const TruthValue& etv = nm->getTV(vt2h(*vtp));
            if (!etv.isNullTv() && etv.getConfidence() > minConfidenceForAbort)
                return *eval_res_vector_set.begin();
        }
        tlog(0, "infer() ok\n");
    }

    const vector<set<VtreeProvider*> >& eval_res_vector_set = variableScoper->GetEvalResults();
    
    assert(!eval_res_vector_set.empty());

    return *eval_res_vector_set.begin();
}

bool BITNode::CreateChildren(int i, BBvtree arg, Btr<bindingsT> bindings, spawn_mode spawning)
{
    assert(!arg->empty());

    tlog(1,"arg #%d. To produce:\n", i);
    rawPrint(*arg, arg->begin(),1); 

    tlog(1,"Creating children...\n");

    if (PREVENT_LOOPS && inferenceLoopWith(arg))
    {
        HypothesisRule hr(::haxx::defaultAtomTableWrapper);
        expandRule(&hr, i, arg, bindings, spawning);
        tlog(-2,"LOOP! Assumed Hypothetically:");
        rawPrint(*arg, arg->begin(), 2);
    }
    else
    {
      foreach(Rule *r, *root->rp)
          expandRule(r, i, arg, bindings, spawning);
    }
    tlog(1,"Rule expansion ok!\n");

    if (children[i].empty())
    {
        tlog(1,"Arg %d proof failure.\n",i);

        return false;
    }
            
    return true;
}

void BITNode::CreateChildrenForAllArgs()
{
    tlog(1,"---CreateChildrenForAllArgs()\n");  
    
    for (uint i = 0; i < args.size(); i++)
        if (!CreateChildren(i, args[i], Btr<bindingsT>(new bindingsT), ALLOW_SIBLING_SPAWNING))
            break;
}

bool BITNode::CheckForDirectResults()
{
    AtomSpace *nm = CogServer::getAtomSpace();
    if (!rule || rule->isComputable())
        return false;

    Handle th = v2h(*GetTarget()->begin());
    if (nm->isReal(th) && nm->getType(th) == FW_VARIABLE_NODE)
    {
        tlog(-1,"Proof of FW_VARIABLE_NODE prohibited.\n");
        return true;
    }

    boost::shared_ptr<set<BoundVertex> > directResult;

    if (USE_GENERATOR_CACHE)
    {
        directProductionArgs dp_args(rule, *bound_target);

        map<directProductionArgs, boost::shared_ptr<set<BoundVertex> >, less_dpargs>::iterator ex_it =
            haxx::DirectProducerCache.find(dp_args);
    
        directResult = ((haxx::DirectProducerCache.end() != ex_it)
            ? ex_it->second
            : haxx::DirectProducerCache[dp_args] = rule->attemptDirectProduction(bound_target));

        if (haxx::DirectProducerCache.end() == ex_it)
            tlog(-1,"attemptDirectProduction. Cache size %d. Target:\n", haxx::DirectProducerCache.size());
        else
            tlog(-1,"CACHED DirectProduction. Cache size %d. Target:\n", haxx::DirectProducerCache.size());
    }
    else
        directResult = rule->attemptDirectProduction(bound_target);

    if (directResult && !directResult->empty())
    {
        addDirectResult(directResult, ALLOW_SIBLING_SPAWNING);              
            
        return true;
    }
    else
    {
        tlog(3,"NO direct child_results generated.\n");
        return false;           
    }
}

void BITNode::expandNextLevel()
{
    AtomSpace *nm = CogServer::getAtomSpace();
  try
  {
     tlog(-2, "Expanding with fitness %.4f   In expansion pool: %s\n", fitness(), (STLhas2(root->exec_pool, this)?"YES":"NO"));
     rawPrint(*GetTarget(), GetTarget()->begin(), -2);
     printArgs();
     if (nm->getType(v2h(*GetTarget()->begin())) == FW_VARIABLE_NODE)    
        tlog(2, "Target is FW_VARIABLE_NODE! Intended? Dunno.\n");
     tlog(0, "Rule:%s: ExpandNextLevel (%d children exist)\n", (rule?(rule->name.c_str()):"(root)"), children.size());
    
    root->exec_pool.remove_if(bind2nd(equal_to<BITNode*>(), this));
    
    if (!Expanded)
    {
        CheckForDirectResults();
        CreateChildrenForAllArgs();
        Expanded = true;
    }
    else
        for (uint i = 0; i < args.size(); i++)
        {   
            foreach(const ParametrizedBITNode& bisse, children[i])
                bisse.prover->expandNextLevel();
            
            if (children[i].empty())
            {
                tlog(1,"Arg %d proof failure.\n",i);        
                break;
            }           
        }       
  } catch(...) { tlog(0,"Exception in ExpandNextLevel()"); throw; }
}



/* Algorithm: Evaluation */



bool BITNode::NotifyParentOfResult(VtreeProvider* new_result) const
{
    AtomSpace *nm = CogServer::getAtomSpace();
    //assert(vt2h(*new_result)->isReal());
    assert(nm->isReal(vt2h(*new_result)));

    stats::Instance().ITN2atom[(BITNode*)this].insert(*new_result->getVtree().begin());

    foreach(const parent_link<BITNode>& p, parents)
        p.link->EvaluateWith(p.parent_arg_i, new_result);

    return true;
}
#if 1
void BITNode::EvaluateWith(unsigned int arg_i, VtreeProvider* new_result)
{
    AtomSpace *nm = CogServer::getAtomSpace();
//  tlog(-1, "ARG %d:\n", arg_i);
//  printTree(v2h(new_result), 0,-1);
    Handle h_new_result = v2h(*new_result->getVtree().begin());
    printArgs();

    /// If any of the existing results are equal in structure and
    /// higher confidence than the new one, skip it.

    foreach(VtreeProvider* old_result, eval_results[arg_i])
        if (IsIdenticalHigherConfidenceAtom(v2h(*old_result->getVtree().begin()), h_new_result))
            return;

    eval_results[arg_i].insert(new_result);

#if FORMULA_CAN_COMPUTE_WITH_EMPTY_ARGS
    foreach(const set<VtreeProvider>& vset, eval_results)
        if (vset.empty())
            return;
#endif

    if (rule)
    {
        vector<Btr<set<VtreeProvider*> > > child_results;

        for (uint i=0;i<args.size();i++)
        {
            child_results.push_back(Btr<set<VtreeProvider*> >(new set<VtreeProvider*>));

            if (i != arg_i)
                *child_results[i] = eval_results[i];
            else
                child_results[i]->insert(new_result);

/*          foreach(const BoundVertex& bv, *miv_set)
                if (bv.bindings)
                    removeRecursionFromHandleHandleMap(bv.bindings); */
        }

        std::set<vector<VtreeProvider*> > argVectorSet;
        int s1 = argVectorSet.empty() ? 0 : argVectorSet.begin()->size();

//      removeRecursion(child_results);
        WithLog_expandVectorSet<vector<VtreeProvider*>,
                                set<VtreeProvider*>,
                                vector<Btr<set<VtreeProvider*> > >::const_iterator >
            (child_results, argVectorSet);

        BoundVertex next_result;

        for (std::set<vector<VtreeProvider*> >::iterator    a = argVectorSet.begin();
                                                    a!= argVectorSet.end();
                                                    a++)
            if (!a->empty() && (rule->hasFreeInputArity() || a->size() >= rule->getInputFilter().size()))
            {
                /// Arg vector size excession prohibited.

                const vector<VtreeProvider*>& rule_args = *a;

                int s2 = rule_args.size();

                RuleApp* ruleApp = (RuleApp*)NULL;
                indirect_iterator<vector<VtreeProvider*>::const_iterator, const VtreeProvider > ii;
            
                ValidateRuleArgs(rule_args.begin(), rule_args.end());

                foreach(VtreeProvider* ra, rule_args)
                {
                    Handle h = v2h(*ra->getVtree().begin());
                    if (!nm->inheritsType(nm->getType(h), HYPOTHETICAL_LINK) &&
                        nm->getTV(h).getConfidence() < MIN_CONFIDENCE_FOR_RULE_APPLICATION)
                        goto next_args;
                }

                tlog(-1, "Evaluating...\n");

                ruleApp = new RuleApp(rule);
                
                next_result = ruleApp->compute(rule_args.begin(), rule_args.end()); //rule->compute(rule_args);

                assert(nm->isReal(v2h(next_result.value)));
                
                ii = rule_args.begin();

                if (ValidRuleResult(next_result,
                        indirect_iterator<vector<VtreeProvider*>::const_iterator, const VtreeProvider>(rule_args.begin()),
                        indirect_iterator<vector<VtreeProvider*>::const_iterator, const VtreeProvider>(rule_args.end()),
                        Btr<bindingsT>(new bindingsT())))
                {
                    NotifyParentOfResult(ruleApp);

                    root->hsource[v2h(next_result.value)] = const_cast<BITNode*>(this);

                    if (RECORD_TRAILS)
                        foreach(VtreeProvider* v, rule_args)
                        {
//                          root->inferred_from[v2h(next_result.value)].push_back(v2h(v.value));
                            haxx::inferred_from[v2h(next_result.value)].push_back(v2h(*v->getVtree().begin()));
//                          root->inferred_with[v2h(next_result.value)] = rule;
                            haxx::inferred_with[v2h(next_result.value)] = rule;
                        }                       
                }
next_args:;
            }
    }
    else
    {
        tlog(-3, "Target produced!\n");

        if (this != root && root)
        {
            foreach(const set<VtreeProvider*>& eval_res_set, eval_results)
                foreach(const parent_link<BITNode>& p, parents)
                    p.link->eval_results[0].insert(eval_res_set.begin(), eval_res_set.end());
        }
            
/*      foreach(const set<VtreeProvider>& eval_res_set, GetEvalResults())
            foreach(const BoundVertex& eval_res, eval_res_set)
                printTree(v2h(eval_res.value),0,-3);
*/
    }

}

/// \todo Currently always uses ForAllRule to generalize. Should be different for varScopeLink
/// \todo Currently return value topology is wrong.

BoundVertex BITNodeRoot::Generalize(Btr<set<BoundVertex> > bvs, Type _resultT) const
{
    vector<Vertex> ForAllArgs;
    BoundVertex new_result((Handle)ATOM);

    const float min_confidence = 0.0001f;

    if (!bvs->empty())
    {
		cprintf(0,"\n");
        tlog(0,"Generalizing results:\n");

        foreach(const BoundVertex& b, *bvs)
            if (getTruthValue(v2h(b.value)).getConfidence() > min_confidence)
            {
                printTree(v2h(b.value),0,0);
                ForAllArgs.push_back(b.value);
            }

        if (_resultT == FORALL_LINK)
            new_result = FORALLRule(::haxx::defaultAtomTableWrapper, (Handle) NULL).compute(ForAllArgs);
        else
            new_result = PLNPredicateRule(::haxx::defaultAtomTableWrapper, NULL).compute(ForAllArgs);

        tlog(0,"\nCombining %d results for final unification. Result was:\n", ForAllArgs.size());
        printTree(v2h(new_result.value),0,0);
    }
    else
        tlog(1,"NO Results for the root query.\n");

    return new_result;
}



/* Fitness evaluation */



int BITNode::number_of_free_variables_in_target() const
{
    AtomSpace *nm = CogServer::getAtomSpace();
    /// Use set<> to prevent re-counting of the already-found Handles
    
    set<Handle> vars;
    
    for(vtree::iterator v  = GetTarget()->begin(); v != GetTarget()->end(); v++)
        if (nm->getType(v2h(*v)) == FW_VARIABLE_NODE)
            vars.insert(v2h(*v));   

    tlog(4,"number_of_free_variables_in_target: %d\n", vars.size());
        
    return (int)vars.size();
}

float BITNode::my_solution_space() const
{
    return counted_number_of_free_variables_in_target - GetTarget()->size()*100.0f;
}

float BITNode::fitness() const
{       
    const float CONFIDENCE_WEIGHT = 10000.0f;
    const float DEPTH_WEIGHT = 100.0f;
    const float SOLUTION_SPACE_WEIGHT = 0.01f;
    const float RULE_PRIORITY_WEIGHT = 0.0001f;

//tlog(3,"fitness(): %f %f %f %f %f\n",my_bdrum, -depth, _bdrum, -1.0f*_bdrum,-depth -_bdrum);  
    
    return  -1.0f*SOLUTION_SPACE_WEIGHT * my_solution_space()
            -1.0f*DEPTH_WEIGHT          * depth
            -1.0f*CONFIDENCE_WEIGHT     * my_bdrum
            +1.0f*RULE_PRIORITY_WEIGHT  * (rule ? rule->getPriority() : 0);
    
/*  \todo Use arity in the spirit of the following:

int missing_arity = (int)rule->getInputFilter().size();
        
        foreach(Btr<set<BoundVertex> > rset, child_results)
        {
            if (!rset->empty())
                missing_arity--;
        }
        
        
        return RULE_PRIORITY_WEIGHT*priority[rule] / (missing_arity+1) / (DEPTH_PRIORITY_WEIGHT*depth)
                - _bdrum;
*/      
}

void BITNode::findFittest(BITNode*& bisse, float& best_fitness)
{
    bisse = (*root->exec_pool.begin());
    best_fitness = bisse->fitness();
    float next_fitness = 0.0f;
    foreach(BITNode* bis, root->exec_pool)
        if (best_fitness < (next_fitness=bis->fitness()))
        {
            best_fitness = next_fitness;
            bisse = bis;
        }
}

float all_best_fitness = 0.0f;

extern float temperature;

void BITNode::expandFittest()
{
    if (SOFTMAX == FitnessEvaluator)
    {
        /*  tlog(2,"Fitness table: (%d)\n", root->exec_pool.size());

        currentDebugLevel=2;

        printFitnessPool();

        currentDebugLevel=-4;
        */
        //////////
        //foreach(BackInferenceTreeNode* b, exec_pool)
        //  partition += 

        vector<double> fitnesses;

        transform(root->exec_pool.begin(), root->exec_pool.end(), back_inserter(fitnesses), mem_fun(&BITNode::fitness));

        //float partition = o;
        //const float temperature = 0.1f;

        int accuracy = RAND_MAX-1; //1000*1000;
        //float selection_weight_coordinate = exp(selection / temperature);

        double total_weight = 0.0, accumulated_weight = 0.0; 


        foreach(double Qb, fitnesses)
        {
            // NOTE! Too low temperature will kill!
            assert(temperature > 0.00001);
//          double a1 = exp((-1.0/Qb) / 0.00001);
            total_weight += exp((-1.0/Qb) / temperature);
        }

        /*time_t seconds;
        time(&seconds);

        srand((unsigned int) seconds);*/

        /*cout<< rand() << endl;
        cout<< rand() << endl;
        cout<< rand() << endl;
        */
        int r = rand();
        double selection = (1.0f/accuracy) * (r%accuracy) * total_weight;

        accumulated_weight = 0.0f; 
        int i=0;
        BITNodeRoot::exec_poolT::iterator ei = root->exec_pool.begin();

        foreach(double Qb, fitnesses)
        {
            //  partition += exp((-1.0/Qb) / temperature);
            accumulated_weight += exp((-1.0/Qb) / temperature);
            if (accumulated_weight > selection)
                break;
            i++;
            ei++;
        }

        // If we didn't reach 'selection' even after the last step, we're in trouble.
        assert(accumulated_weight > selection);
        /*
        printf("EXPANDING:  %.8f / %.8f / %.8f/ %.8f/ %d\n", selection, exp((-1.0f/fitnesses[i]) / temperature), accumulated_weight,
        total_weight, r);
        (*ei)->tlog(-4, ": %f / %d [%d]\n", (*ei)->fitness(), (*ei)->children.size(), (int)(*ei));
        */
        //getc(stdin);
        (*ei)->expandNextLevel();

        return;
    }
    else if (FitnessEvaluator == RANDOM)
    {
        BITNodeRoot::exec_poolT::iterator ei = root->exec_pool.begin();
        int e = rand() % root->exec_pool.size();
        for (int i=0; i < e; i++)
            ++ei;

        (*ei)->expandNextLevel();
    }
    else
    {
        print_progress();

        BITNode* bisse = NULL;
    
        if (!root->exec_pool.empty())
        {
            if (currentDebugLevel>1) //Sort and print
            {
                if (!root->exec_pool_sorted)
                {
                    root->exec_pool.sort(BITNode_fitness_comp());
                    root->exec_pool_sorted = true;
                }

                list<BITNode*>::iterator i = root->exec_pool.begin();
                (*i)->tlog(3, ": %f / %d [%ld]\n", (*i)->fitness(), (*i)->children.size(), (long)(*i));

                for (++i; i != root->exec_pool.end(); i++)      
                    (*i)->tlog(3, ": %f / %d [%ld]\n", (*i)->fitness(), (*i)->children.size(), (long)(*i));

                bisse = (*root->exec_pool.begin());
            }
            else //Just find the best one
            {
                float best_fitness = 0.0f;
                findFittest(bisse, best_fitness);
                if (all_best_fitness > best_fitness)
                    all_best_fitness = best_fitness;

				cprintf(-2, "\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b%.4f / %.4f", all_best_fitness, best_fitness);
            }

            bisse->expandNextLevel();
        }
    }
}

bool BITNode_fitness_comp::operator()(BITNode* lhs, BITNode* rhs) const
{
//return lhs>rhs;
    
    if (!lhs)
        return false;
    if (!rhs)
        return true;

    float lfit = lhs->fitness();
    float rfit = rhs->fitness();
    float fit_diff = lfit - rfit;
//printf("%f_", fit_diff);  
    const float fitness_epsilon = 0.00000001f;
    return (fit_diff > fitness_epsilon ||
            ((fabs(fit_diff) < fitness_epsilon) && lhs>rhs));
}



/* Action */


void BITNodeRoot::extract_plan(Handle h, unsigned int level, vtree& do_template, vector<Handle>& plan) const
{
    AtomSpace *nm = CogServer::getAtomSpace();
    map<Handle, vtree> bindings;
    
    if (!h || !nm->isReal(h))
        puts("NULL / Virtual? Syntax: t<enter> Handle#<enter>");
    
    map<Handle,Rule*> ::const_iterator rule = haxx::inferred_with.find(h);
    
    if (rule != haxx::inferred_with.end())
    {
        foreach(Handle arg_h, haxx::inferred_from[h])
        {
            if (unifiesTo(do_template, make_vtree((Handle) arg_h), bindings, bindings, true))
            {
                puts("Satisfies do_template:");
                printTree(arg_h,level+1,0);
                plan.push_back(arg_h);
            }
        
            extract_plan(arg_h, level+1, do_template, plan);
        }
    }
}

void BITNodeRoot::extract_plan(Handle h) const
{
    AtomSpace *nm = CogServer::getAtomSpace();
    vtree do_template = mva((Handle)EVALUATION_LINK,
                            NewNode(PREDICATE_NODE, "do"),
                            mva((Handle)LIST_LINK,
                                NewNode(FW_VARIABLE_NODE, "$999999999")));

    vector<Handle> plan;
    extract_plan(h,0,do_template,plan);
puts("PLAN BEGIN");
    for (vector<Handle>::reverse_iterator i = plan.rbegin(); i!=plan.rend(); i++)
        printTree(*i,0,-10);
puts("PLAN END");   
    if (plan.size()>0)
    {
        puts("[plan found, exiting");
        assert(0);
    }
}

static set<BITNode*> loop_t;
void BITNode::LoopCheck() const
{
    loop_t.insert((BITNode*)(long)this);
//  printf("\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b%d / %d\n", loop_t.size(), root->InferenceNodes);
    printf("%d %ld:     %d\n", depth, (long)this,  TotalChildren());
    getc(stdin);
    for (vector<set<ParametrizedBITNode> >::const_iterator i =  children.begin(); i!=children.end(); i++)
    {
        foreach(const ParametrizedBITNode& pbit, *i)
            pbit.prover->LoopCheck();
    }
}

#if 0
void test_pool_policy()
{
/*  Rule* deductionR1 = RuleRepository::Instance().rule[Deduction_Implication];
    Rule* deductionR2 = RuleRepository::Instance().rule[Deduction_Inheritance];
*/
    Rule* deductionR2 = NULL; //Should have no effect

    assert(ObeysPoolPolicy(deductionR2,
        Btr<tree<Vertex> > (new tree<Vertex>(mva((Handle)EVALUATION_LINK,
                    NewNode(PREDICATE_NODE, "killed"),
                    mva((Handle)LIST_LINK,
                                NewNode(FW_VARIABLE_NODE, "$killeri"),
                                NewNode(CONCEPT_NODE, "Osama")
                            )
            )))));

    assert(!ObeysPoolPolicy(deductionR2,
        Btr<tree<Vertex> > (new tree<Vertex>(mva((Handle)EVALUATION_LINK,
                    NewNode(PREDICATE_NODE, "killed"),
                    mva((Handle)LIST_LINK,
                                NewNode(FW_VARIABLE_NODE, "$killeri"),
                                NewNode(FW_VARIABLE_NODE, "$target")
                            )
            )))));

    assert(ObeysPoolPolicy(deductionR2,
        Btr<tree<Vertex> > (new tree<Vertex>(mva((Handle)EVALUATION_LINK,
                    NewNode(FW_VARIABLE_NODE, "$action"),,
                    mva((Handle)LIST_LINK,
                                NewNode(FW_VARIABLE_NODE, "$killeri"),
                                NewNode(CONCEPT_NODE, "Osama")
                            )
            )))));

    puts("Pool policy test ok!");
}

#endif

/* PRINTING METHODS */

void BITNode::printResults() const
{
    foreach(const set<VtreeProvider*>& vset, eval_results)
    {
        printf("[ ");
        foreach(VtreeProvider* vtp, vset)
            printTree(v2h(*vtp->getVtree().begin()),0,-10);
//          printf("%d ", v2h(bv.value));
        printf("\n");
    }
}

void BITNode::print(int loglevel, bool compact, Btr<set<BITNode*> > UsedBITNodes) const
{
//  if (HasAncestor(this))
//      return;

    if (UsedBITNodes == NULL)
        UsedBITNodes = Btr<set<BITNode*> >(new set<BITNode*>());

    //if (currentDebugLevel>=-12 && !((more_count++)%25))
    //{
    //  puts(" --- more ");
    //  if (getc(stdin) == 'q')
    //      return;
    //}

    #define prlog cprintf

    if (rule)
    {
        if (compact)
			prlog(loglevel, "%s%s\n", repeatc(' ', depth*3).c_str(), rule->name.c_str());
        else
        {
            string cbuf("[ ");
            if (direct_results)
                foreach(const BoundVertex& bv, *direct_results)
                cbuf += i2str((int)v2h(bv.value)) + " ";
            if (cbuf.empty())
                cbuf = "[]";
            prlog(loglevel,"%s%s ([%ld])\n", repeatc(' ', depth*3).c_str(), rule->name.c_str(), (long)this);
            prlog(loglevel,"%s%s]\n", repeatc(' ', (depth+1)*3).c_str(), cbuf.c_str());
        }
    }
    else
		prlog(loglevel,"root\n");
    
    if (STLhas2(*UsedBITNodes, this))
		prlog(loglevel,  "%s(loop)\n", repeatc(' ', (depth+1)*3).c_str());
    else
    {
        UsedBITNodes->insert((BITNode*)this);

        int ccount=0;
        for (vector<set<ParametrizedBITNode> >::const_iterator i =  children.begin(); i!=children.end(); i++)
        {
            if (compact)
				prlog(loglevel,  "%s#%d\n", repeatc(' ', (depth+1)*3).c_str(), ccount++);
            else
            {
				prlog(loglevel, "%sARG #%d:\n", repeatc(' ', (depth+1)*3).c_str(), ccount);
				prlog(loglevel, "%s---\n", repeatc(' ', (depth+1)*3).c_str());
                ccount++;
            }

            int n_children = i->size();

            foreach(const ParametrizedBITNode& pbit, *i)
            {
                //cprintf(loglevel,"::: %d\n", (int)pbit.prover);
                if (!compact || STLhas(stats::Instance().ITN2atom, pbit.prover))
                    pbit.prover->print(loglevel, compact, UsedBITNodes);
            }
        }
//      UsedBITNodes->erase((BITNode*)(int)this);
    }
}

static int _trail_print_more_count = 0;

void BITNodeRoot::print_trail(Handle h, unsigned int level) const //, int decimal_places) const
{
    AtomSpace *nm = CogServer::getAtomSpace();
    if (!h || !nm->isReal(h))
        puts("NULL / Virtual? Syntax: t<enter> Handle#<enter>");
    map<Handle,Rule*> ::const_iterator rule = haxx::inferred_with.find(h);
    if (rule != haxx::inferred_with.end())
    {
        printf("%s[%ld] was produced by applying %s to:\n", repeatc(' ', level*3).c_str(), (long)h, rule->second->name.c_str());
        map<Handle,vector<Handle> >::const_iterator h_it = haxx::inferred_from.find(h);
        assert (h_it != haxx::inferred_from.end());
        foreach(Handle arg_h, h_it->second)
        {
          // If the handle is used in other places too, then and only then print it's id.
          typedef pair<Handle, vector<Handle> > hvhT;
          int h_use_count = 0;
          if (haxx::inferred_from.find(arg_h) != haxx::inferred_from.end())
            h_use_count=2;
          /*          foreach(const hvhT& hvh, inferred_from)
            if (find(hvh.second.begin(), hvh.second.end(), arg_h)
            != hvh.second.end())
              h_use_count++;*/
        NMPrinter(NMP_ALL, 2, NM_PRINTER_DEFAULT_INDENTATION_TAB_SIZE, 0, level+1).print(arg_h, -10);
//      NMPrinter(((h_use_count>1)?NMP_HANDLE:0)|NMP_TYPE_NAME|NMP_NODE_NAME| NMP_NODE_TYPE_NAME|NMP_TRUTH_VALUE, 2, NM_PRINTER_DEFAULT_INDENTATION_TAB_SIZE, 0, level+1).print(arg_h, -10);
            print_trail(arg_h, level+1);
        }
    }
    else
        cout << repeatc(' ', level*3) << "which is trivial (or axiom).\n";
#if 0 // "(more)"
    if ((_trail_print_more_count++)%5 == 4)
    {
        cout << "(more)\n";
        getc(stdin);
    }
#endif
}

void BITNode::printArgs() const
{
    cprintf(-2,"%u args:\n", (unsigned int) args.size());
//  for_each(args.begin(), args.end(), NMPrinter(NMP_ALL));
   foreach(meta _arg, args)
      NMPrinter(NMP_ALL).print(*_arg, -2);

//      rawPrint(*_arg, _arg->begin(), -2);
}

void BITNode::printFitnessPool()
{
    tlog(0,"Fitness table: (%d)\n", root->exec_pool.size());
    getc(stdin);getc(stdin);

    if (!root->exec_pool.empty())
    {
        if (!root->exec_pool_sorted)
        {
            root->exec_pool.sort(BITNode_fitness_comp());
            root->exec_pool_sorted = true;
        }

        for (list<BITNode*>::iterator i = root->exec_pool.begin(); i != root->exec_pool.end(); i++)     
        {
            (*i)->tlog(0, ": %f / %d [%ld] (P = %d)\n", (*i)->fitness(), (*i)->children.size(), (long)(*i), (*i)->parents.size());

            if (currentDebugLevel>=2 && !((more_count++)%25))
            {
                puts(" --- more ");
                getc(stdin);
            }
        }
    }
}

void BITNodeRoot::print_trail(Handle h) const
{
    printTree(h,0,0);
    print_trail(h,0);
}

static bool bigcounter = true;

int BITNode::tlog(int debugLevel, const char *format, ...) const
    {
        if (debugLevel > currentDebugLevel) return 0;

         if (test::bigcount == 601)
         { puts("Debug feature."); }
        
         printf("%ld %u/%ld %d [(%ld)] (%s): ", (bigcounter?(++test::bigcount):depth),
                 (unsigned int) root->exec_pool.size(), root->InferenceNodes, haxxUsedProofResources,
//          (test::LOG_WITH_NODE_ID ? ((int)this) : 0),
            (long)this, (rule ? (rule->name.c_str()) : "(root)"));
        
        if (test::logfile)
            fprintf(test::logfile, "%d %ld [(%ld)] (%s): ", depth, root->InferenceNodes,
//          (test::LOG_WITH_NODE_ID ? ((int)this) : 0),
            (long)this,
            (rule ? (rule->name.c_str()) : "(root)"));
        
        char buf[5000];
        
        va_list ap;
        va_start(ap, format);
        int answer = vsprintf(buf, format, ap);
        
        printf(buf);
        
        if (test::logfile)
            fprintf(test::logfile, buf);
        
        fflush(stdout);
        va_end(ap);
        return answer;
    }

void BITNode::printChildrenSizes() const
    {
		if (currentDebugLevel>=3)
			puts("next chi...0");
        tlog(3,"next chi...0");
        for(uint c=0; c< children.size(); c++)
        {
            tlog(3,"(%d:%d), ", c, children[c].size());
            tlog(3,"\n");
        }
    }

void BITNode::printTarget() const
{
	cprintf(0, "Raw target:\n");
	rawPrint(*raw_target, raw_target->begin(),0);
	cprintf(0, "Bound target:\n");
	rawPrint(*GetTarget(), GetTarget()->begin(),0);
}


/*
/// No longer needed? MAY NOT BE UP2DATE:
/// Children are also cloned, but results (gained so far) will be shared instead.
BITNode* BITNode::Clone() const
    {
        BITNode* ret = new BITNode(*this);
        InferenceNodes++; 
        
        vector<set<BITNode*> > new_children;
        
        for (vector<set<BITNode*> >::iterator childi = ret->children.begin();
                childi != ret->children.end(); childi++)
        {
//      foreach(set<BITNode*>& child, ret->children)
            set<BITNode*> new_c_set;
            
            //for (set<BITNode*>::iterator bit=childi->begin();bit!=childi->end();bit++)
            foreach(BITNode* const& bitree, *childi)
                new_c_set.insert(bitree->Clone());
            
            new_children.push_back(new_c_set);
        }
        
        ret->children = new_children;
        
        ret->target = Btr<vtree>(new vtree(*target));
        foreach(meta m, args)
            ret->args.push_back(Btr<vtree>(new vtree(*m)));

        return ret;     
    }

struct target_binder
    {       
        bindingsT bindings;
        target_binder(const bindingsT& _bindings) : bindings(_bindings) {}
        void operator()(BITNode* b)
        {
            cprintf(3,make_subst_buf(bindings).c_str());
                        
                meta target = b->GetTarget();

                cprintf(3,"Before bind:");
                rawPrint(*target,target->begin(),3);
            
                b->SetTarget(bind_vtree(*target, bindings));
                
                cprintf(3,"After:");
                rawPrint(*b->GetTarget(),b->GetTarget()->begin(),3);
        }
    };
    
BITNode* BITNode::Bind(bindingsT b)
    {
        ApplyDown(target_binder(b));
        return this;        
    }
*/  

/*#ifdef WIN32

bool indirect_less_BITNode::operator()(BITNode* lhs, BITNode* rhs) const
{
    if (lhs->rule < rhs->rule)
        return true;
    else if (lhs->rule > rhs->rule)
        return false;

    if (lhs->args.size() < rhs->args.size())
        return true;
    else if (lhs->args.size() > rhs->args.size())
        return false;

    /// If target-determined
    if (lhs->rule && !lhs->rule->IsComputable())
    {
        if (less_vtree()(*lhs->GetTarget(), *rhs->GetTarget()))
            return true;
        else if (less_vtree()(*rhs->GetTarget(), *lhs->GetTarget()))
            return false;
    }
    else
    {
        for(int i=0; i<lhs->args.size(); i++)
            if (less_vtree()(lhs->args[i]->getStdTree(), rhs->args[i]->std_tree()))
                return true;
            else if (less_vtree()(rhs->args[i]->getStdTree(), lhs->args[i]->std_tree()))
                return false;
    }

    return false;
}

struct indirect_less_BITNode : public binary_function<BITNode*, BITNode*, bool>
{
    bool operator()(BITNode* lhs, BITNode* rhs) const;
};

typedef set<BITNode*, indirect_less_BITNode> BITNodestoreT;

struct BITNodehash :  public stdext::hash_compare<BITNode*>
{
    /// hash function
    size_t operator()(BITNode* b) const
    {
        size_t ret = 0;
        ret += (int)b->rule;

        ret += BoundVTree(*b->GetTarget()).getFingerPrint();

        foreach(Btr<BoundVTree> bvt, b->args)
            ret += bvt->getFingerPrint();

        return ret;
    }
    bool operator()(BITNode* lhs, BITNode* rhs) const
    {
        if (lhs->rule < rhs->rule)
            return true;
        else if (lhs->rule > rhs->rule)
            return false;

        if (lhs->args.size() < rhs->args.size())
            return true;
        else if (lhs->args.size() > rhs->args.size())
            return false;

        /// If target-determined
        if (lhs->rule && !lhs->rule->IsComputable())
        {
            /// \todo Should look at std_tree forms of targets here!

            if (less_vtree()(*lhs->GetTarget(), *rhs->GetTarget()))
                return true;
            else if (less_vtree()(*rhs->GetTarget(), *lhs->GetTarget()))
                return false;
        }
        else
        {
            for(int i=0; i<lhs->args.size(); i++)
                if (less_vtree()(lhs->args[i]->getStdTree(), rhs->args[i]->std_tree()))
                    return true;
                else if (less_vtree()(rhs->args[i]->getStdTree(), lhs->args[i]->std_tree()))
                    return false;
        }

        return false;
    }
};

#include <hash_set>

typedef hash_set<BITNode*, BITNodehash> BITNodestoreT;

#else*/

#if 0
template<typename T1, typename T2, typename T3>
void combine_maps(const map<T1, T2>& m12, const map<T2, T3>& m23, map<T1, T2>& m13)
{
    typedef pair<T1, T2> p12T;
    typedef pair<T2, T3> p23T;
    foreach(p12T p12, m12)
    {
        map<T2,T3>::const_iterator it23f = m23.find(p12.first);
        map<T2,T3>::const_iterator it23s = m23.find(p12.second);

        m13[  ((m23.end() == it23f) ? p12.first  : it23f->second)]
            = ((m23.end() == it23s) ? p12.second : it23s->second);
    }
}
#endif

    /*
class priorityComp :  public std::binary_function<Rule*,Rule*,bool>
{
    const std::map<Rule*, float>& priority;
public:
    priorityComp(const std::map<Rule*, float>& _priority) : priority(_priority)
    {}

    bool operator()(Rule* x, Rule* y) //true if x has higher priority
    {
        std::map<Rule*, float>::const_iterator xi, yi;

        if ((yi=priority.find(y)) == priority.end())
            return true;
        else if ((xi=priority.find(x)) == priority.end())
            return false;
        else
            return (xi->second > yi->second);
    }
};
    
*/

#endif

} //namespace reasoning




