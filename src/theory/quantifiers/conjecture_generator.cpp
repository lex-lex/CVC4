/*********************                                                        */
/*! \file subgoal_generator.cpp
 ** \verbatim
 ** Original author: Andrew Reynolds
 ** Major contributors: none
 ** Minor contributors (to current version): none
 ** This file is part of the CVC4 project.
 ** Copyright (c) 2009-2014  New York University and The University of Iowa
 ** See the file COPYING in the top-level source directory for licensing
 ** information.\endverbatim
 **
 ** \brief conjecture generator class
 **
 **/

#include "theory/quantifiers/conjecture_generator.h"
#include "theory/theory_engine.h"
#include "theory/quantifiers/options.h"
#include "theory/quantifiers/term_database.h"
#include "theory/quantifiers/trigger.h"
#include "theory/quantifiers/first_order_model.h"

using namespace CVC4;
using namespace CVC4::kind;
using namespace CVC4::theory;
using namespace CVC4::theory::quantifiers;
using namespace std;


namespace CVC4 {

void OpArgIndex::addTerm( ConjectureGenerator * s, TNode n, unsigned index ){
  if( index==n.getNumChildren() ){
    Assert( n.hasOperator() );
    if( std::find( d_ops.begin(), d_ops.end(), n.getOperator() )==d_ops.end() ){
      d_ops.push_back( n.getOperator() );
      d_op_terms.push_back( n );
    }
  }else{
    d_child[s->getTermDatabase()->d_arg_reps[n][index]].addTerm( s, n, index+1 );
  }
}

Node OpArgIndex::getGroundTerm( ConjectureGenerator * s, std::vector< TNode >& args ) {
  if( d_ops.empty() ){
    for( std::map< TNode, OpArgIndex >::iterator it = d_child.begin(); it != d_child.end(); ++it ){
      std::map< TNode, Node >::iterator itf = s->d_ground_eqc_map.find( it->first );
      if( itf!=s->d_ground_eqc_map.end() ){
        args.push_back( itf->second );
        Node n = it->second.getGroundTerm( s, args );
        args.pop_back();
        if( !n.isNull() ){
          return n;
        }
      }
    }
    return Node::null();
  }else{
    std::vector< TNode > args2;
    args2.push_back( d_ops[0] );
    args2.insert( args2.end(), args.begin(), args.end() );
    return NodeManager::currentNM()->mkNode( d_op_terms[0].getKind(), args2 );
  }
}

void OpArgIndex::getGroundTerms( ConjectureGenerator * s, std::vector< TNode >& terms ) {
  terms.insert( terms.end(), d_op_terms.begin(), d_op_terms.end() );
  for( std::map< TNode, OpArgIndex >::iterator it = d_child.begin(); it != d_child.end(); ++it ){
    if( s->isGroundEqc( it->first ) ){
      it->second.getGroundTerms( s, terms );
    }
  }
}



ConjectureGenerator::ConjectureGenerator( QuantifiersEngine * qe, context::Context* c ) : QuantifiersModule( qe ),
d_notify( *this ),
d_uequalityEngine(d_notify, c, "ConjectureGenerator::ee"),
d_ee_conjectures( c ){
  d_fullEffortCount = 0;
  d_uequalityEngine.addFunctionKind( kind::APPLY_UF );
  d_uequalityEngine.addFunctionKind( kind::APPLY_CONSTRUCTOR );
  
}

void ConjectureGenerator::eqNotifyNewClass( TNode t ){
  Trace("thm-ee-debug") << "UEE : new equivalence class " << t << std::endl;
  d_upendingAdds.push_back( t );
}

void ConjectureGenerator::eqNotifyPreMerge(TNode t1, TNode t2) {
  //get maintained representatives
  TNode rt1 = t1;
  TNode rt2 = t2;
  std::map< Node, EqcInfo* >::iterator it1 = d_eqc_info.find( t1 );
  if( it1!=d_eqc_info.end() && !it1->second->d_rep.get().isNull() ){
    rt1 = it1->second->d_rep.get();
  }
  std::map< Node, EqcInfo* >::iterator it2 = d_eqc_info.find( t2 );
  if( it2!=d_eqc_info.end() && !it2->second->d_rep.get().isNull() ){
    rt2 = it2->second->d_rep.get();
  }
  Trace("thm-ee-debug") << "UEE : equality holds : " << t1 << " == " << t2 << std::endl;
  Trace("thm-ee-debug") << "      ureps : " << rt1 << " == " << rt2 << std::endl;
  Trace("thm-ee-debug") << "      normal : " << d_pattern_is_normal[rt1] << " " << d_pattern_is_normal[rt2] << std::endl;
  Trace("thm-ee-debug") << "      size :   " << d_pattern_fun_sum[rt1] << " " << d_pattern_fun_sum[rt2] << std::endl;

  if( isUniversalLessThan( rt2, rt1 ) ){
    EqcInfo * ei;
    if( it1==d_eqc_info.end() ){
      ei = getOrMakeEqcInfo( t1, true );
    }else{
      ei = it1->second;
    }
    ei->d_rep = t2;
  }
}

void ConjectureGenerator::eqNotifyPostMerge(TNode t1, TNode t2) {

}

void ConjectureGenerator::eqNotifyDisequal(TNode t1, TNode t2, TNode reason) {
  Trace("thm-ee-debug") << "UEE : disequality holds : " << t1 << " != " << t2 << std::endl;

}


ConjectureGenerator::EqcInfo::EqcInfo( context::Context* c ) : d_rep( c, Node::null() ){

}

ConjectureGenerator::EqcInfo* ConjectureGenerator::getOrMakeEqcInfo( TNode n, bool doMake ) {
  //Assert( getUniversalRepresentative( n )==n );
  std::map< Node, EqcInfo* >::iterator eqc_i = d_eqc_info.find( n );
  if( eqc_i!=d_eqc_info.end() ){
    return eqc_i->second;
  }else if( doMake ){
    EqcInfo* ei = new EqcInfo( d_quantEngine->getSatContext() );
    d_eqc_info[n] = ei;
    return ei;
  }else{
    return NULL;
  }
}

void ConjectureGenerator::doPendingAddUniversalTerms() {
  //merge all pending equalities
  while( !d_upendingAdds.empty() ){
    Trace("sg-pending") << "Add " << d_upendingAdds.size() << " pending terms..." << std::endl;
    std::vector< Node > pending;
    pending.insert( pending.end(), d_upendingAdds.begin(), d_upendingAdds.end() );
    d_upendingAdds.clear();
    for( unsigned i=0; i<pending.size(); i++ ){
      Node t = pending[i];
      TypeNode tn = t.getType();
      Trace("thm-ee-add") << "UEE : Add universal term " << t << std::endl;
      //get all equivalent terms from conjecture database
      std::vector< Node > eq_terms;
      d_thm_index.getEquivalentTerms( t, eq_terms );
      if( !eq_terms.empty() ){
        Trace("thm-ee-add") << "UEE : Based on theorem database, it is equivalent to " << eq_terms.size() << " terms : " << std::endl;
        //add equivalent terms as equalities to universal engine
        for( unsigned i=0; i<eq_terms.size(); i++ ){
          Trace("thm-ee-add") << "  " << eq_terms[i] << std::endl;
          //if( d_urelevant_terms.find( eq_terms[i] )!=d_urelevant_terms.end() ){
          bool assertEq = false;
          if( d_urelevant_terms.find( eq_terms[i] )!=d_urelevant_terms.end() ){
            assertEq = true;
          }else{
            Assert( eq_terms[i].getType()==tn );
            registerPattern( eq_terms[i], tn );
            if( isUniversalLessThan( eq_terms[i], t ) ){
              setUniversalRelevant( eq_terms[i] );
              assertEq = true;
            }
          }
          if( assertEq ){
            Node exp;
            d_uequalityEngine.assertEquality( t.eqNode( eq_terms[i] ), true, exp );
          }
        }
      }else{
        Trace("thm-ee-add") << "UEE : No equivalent terms." << std::endl;
      }
    }
  }
}

void ConjectureGenerator::setUniversalRelevant( TNode n ) {
  //add pattern information
  registerPattern( n, n.getType() );
  d_urelevant_terms[n] = true;
  for( unsigned i=0; i<n.getNumChildren(); i++ ){
    setUniversalRelevant( n[i] );
  }
}

bool ConjectureGenerator::isUniversalLessThan( TNode rt1, TNode rt2 ) {
  //prefer the one that is (normal, smaller) lexographically
  Assert( d_pattern_is_normal.find( rt1 )!=d_pattern_is_normal.end() );
  Assert( d_pattern_is_normal.find( rt2 )!=d_pattern_is_normal.end() );
  Assert( d_pattern_fun_sum.find( rt1 )!=d_pattern_fun_sum.end() );
  Assert( d_pattern_fun_sum.find( rt2 )!=d_pattern_fun_sum.end() );
  
  if( d_pattern_is_normal[rt1] && !d_pattern_is_normal[rt2] ){
    Trace("thm-ee-debug") << "UEE : LT due to normal." << std::endl;
    return true;
  }else if( d_pattern_is_normal[rt1]==d_pattern_is_normal[rt2] ){
    if( d_pattern_fun_sum[rt1]<d_pattern_fun_sum[rt2] ){
      Trace("thm-ee-debug") << "UEE : LT due to size." << std::endl;
      //decide which representative to use : based on size of the term
      return true;
    }else if( d_pattern_fun_sum[rt1]==d_pattern_fun_sum[rt2] ){
      //same size : tie goes to term that has already been reported
      return isReportedCanon( rt1 ) && !isReportedCanon( rt2 );
    }
  }
  return false;
}


bool ConjectureGenerator::isReportedCanon( TNode n ) { 
  return std::find( d_ue_canon.begin(), d_ue_canon.end(), n )==d_ue_canon.end(); 
}

void ConjectureGenerator::markReportedCanon( TNode n ) {
  if( !isReportedCanon( n ) ){
    d_ue_canon.push_back( n );
  }
}

bool ConjectureGenerator::areUniversalEqual( TNode n1, TNode n2 ) {
  return n1==n2 || ( d_uequalityEngine.hasTerm( n1 ) && d_uequalityEngine.hasTerm( n2 ) && d_uequalityEngine.areEqual( n1, n2 ) );
}

bool ConjectureGenerator::areUniversalDisequal( TNode n1, TNode n2 ) {
  return n1!=n2 && d_uequalityEngine.hasTerm( n1 ) && d_uequalityEngine.hasTerm( n2 ) && d_uequalityEngine.areDisequal( n1, n2, false );
}

TNode ConjectureGenerator::getUniversalRepresentative( TNode n, bool add ) {
  if( add ){
    if( d_urelevant_terms.find( n )==d_urelevant_terms.end() ){
      setUniversalRelevant( n );
      //add term to universal equality engine
      d_uequalityEngine.addTerm( n );
      Trace("thm-ee-debug") << "Merge equivalence classes based on terms..." << std::endl;
      doPendingAddUniversalTerms();
    }
  }
  if( d_uequalityEngine.hasTerm( n ) ){
    Node r = d_uequalityEngine.getRepresentative( n );
    EqcInfo * ei = getOrMakeEqcInfo( r );
    if( ei && !ei->d_rep.get().isNull() ){
      return ei->d_rep.get();
    }else{
      return r;
    }
  }else{
    return n;
  }
}

eq::EqualityEngine * ConjectureGenerator::getEqualityEngine() {
  return d_quantEngine->getTheoryEngine()->getMasterEqualityEngine();
}

bool ConjectureGenerator::areEqual( TNode n1, TNode n2 ) {
  eq::EqualityEngine * ee = getEqualityEngine();
  return n1==n2 || ( ee->hasTerm( n1 ) && ee->hasTerm( n2 ) && ee->areEqual( n1, n2 ) );
}

bool ConjectureGenerator::areDisequal( TNode n1, TNode n2 ) {
  eq::EqualityEngine * ee = getEqualityEngine();
  return n1!=n2 && ee->hasTerm( n1 ) && ee->hasTerm( n2 ) && ee->areDisequal( n1, n2, false );
}

TNode ConjectureGenerator::getRepresentative( TNode n ) {
  eq::EqualityEngine * ee = getEqualityEngine();
  if( ee->hasTerm( n ) ){
    return ee->getRepresentative( n );
  }else{
    return n;
  }
}

TermDb * ConjectureGenerator::getTermDatabase() {
  return d_quantEngine->getTermDatabase();
}

bool ConjectureGenerator::needsCheck( Theory::Effort e ) {
  if( e==Theory::EFFORT_FULL ){
    //d_fullEffortCount++;
    return d_fullEffortCount%optFullCheckFrequency()==0;
  }else{
    return false;
  }
}

void ConjectureGenerator::reset_round( Theory::Effort e ) {

}

Node ConjectureGenerator::getFreeVar( TypeNode tn, unsigned i ) {
  Assert( !tn.isNull() );
  while( d_free_var[tn].size()<=i ){
    std::stringstream oss;
    oss << tn;
    std::stringstream os;
    os << oss.str()[0] << i;
    Node x = NodeManager::currentNM()->mkBoundVar( os.str().c_str(), tn );
    d_free_var_num[x] = d_free_var[tn].size();
    d_free_var[tn].push_back( x );
  }
  return d_free_var[tn][i];
}



Node ConjectureGenerator::getCanonicalTerm( TNode n, std::map< TypeNode, unsigned >& var_count, std::map< TNode, TNode >& subs ) {
  Trace("ajr-temp") << "get canonical term " << n << " " << n.getKind() << " " << n.hasOperator() << std::endl;
  if( n.getKind()==BOUND_VARIABLE ){
    std::map< TNode, TNode >::iterator it = subs.find( n );
    if( it==subs.end() ){
      TypeNode tn = n.getType();
      //allocate variable
      unsigned vn = var_count[tn];
      var_count[tn]++;
      subs[n] = getFreeVar( tn, vn );
      return subs[n];
    }else{
      return it->second;
    }
  }else{
    std::vector< Node > children;
    if( n.getKind()!=EQUAL ){
      if( n.hasOperator() ){
        TNode op = n.getOperator();
        if( std::find( d_funcs.begin(), d_funcs.end(), op )==d_funcs.end() ){
          return Node::null();
        }
        children.push_back( op );
      }else{
        return Node::null();
      }
    }
    for( unsigned i=0; i<n.getNumChildren(); i++ ){
      Node cn = getCanonicalTerm( n[i], var_count, subs );
      if( cn.isNull() ){
        return Node::null();
      }else{
        children.push_back( cn );
      }
    }
    return NodeManager::currentNM()->mkNode( n.getKind(), children );
  }
}

bool ConjectureGenerator::isHandledTerm( TNode n ){
  return !n.getAttribute(NoMatchAttribute()) && inst::Trigger::isAtomicTrigger( n ) && ( n.getKind()!=APPLY_UF || n.getOperator().getKind()!=SKOLEM );
}

Node ConjectureGenerator::getGroundEqc( TNode r ) {
  std::map< TNode, Node >::iterator it = d_ground_eqc_map.find( r );
  return it!=d_ground_eqc_map.end() ? it->second : Node::null();
}

bool ConjectureGenerator::isGroundEqc( TNode r ) {
  return d_ground_eqc_map.find( r )!=d_ground_eqc_map.end();
}

bool ConjectureGenerator::isGroundTerm( TNode n ) {
  return std::find( d_ground_terms.begin(), d_ground_terms.end(), n )!=d_ground_terms.end();
}

void ConjectureGenerator::check( Theory::Effort e ) {
  if( e==Theory::EFFORT_FULL ){
    bool doCheck = d_fullEffortCount%optFullCheckFrequency()==0;
    if( d_quantEngine->hasAddedLemma() ){
     doCheck = false;
    }else{
      d_fullEffortCount++;
    }
    if( doCheck ){
      Trace("sg-engine") << "---Subgoal engine, effort = " << e << "--- " << std::endl;
      eq::EqualityEngine * ee = getEqualityEngine();

      Trace("sg-proc") << "Get eq classes..." << std::endl;
      d_op_arg_index.clear();
      d_ground_eqc_map.clear();
      d_bool_eqc[0] = Node::null();
      d_bool_eqc[1] = Node::null();
      std::vector< TNode > eqcs;
      d_em.clear();
      eq::EqClassesIterator eqcs_i = eq::EqClassesIterator( ee );
      while( !eqcs_i.isFinished() ){
        TNode r = (*eqcs_i);
        eqcs.push_back( r );
        if( r.getType().isBoolean() ){
          if( areEqual( r, getTermDatabase()->d_true ) ){
            d_ground_eqc_map[r] = getTermDatabase()->d_true;
            d_bool_eqc[0] = r;
          }else if( areEqual( r, getTermDatabase()->d_false ) ){
            d_ground_eqc_map[r] = getTermDatabase()->d_false;
            d_bool_eqc[1] = r;
          }
        }
        d_em[r] = eqcs.size();
        eq::EqClassIterator ieqc_i = eq::EqClassIterator( r, ee );
        while( !ieqc_i.isFinished() ){
          TNode n = (*ieqc_i);
          if( isHandledTerm( n ) ){
            d_op_arg_index[r].addTerm( this, n );
          }
          ++ieqc_i;
        }
        ++eqcs_i;
      }
      Assert( !d_bool_eqc[0].isNull() );
      Assert( !d_bool_eqc[1].isNull() );
      d_urelevant_terms.clear();
      Trace("sg-proc") << "...done get eq classes" << std::endl;

      Trace("sg-proc") << "Determine ground EQC..." << std::endl;
      bool success;
      do{
        success = false;
        for( unsigned i=0; i<eqcs.size(); i++ ){
          TNode r = eqcs[i];
          if( d_ground_eqc_map.find( r )==d_ground_eqc_map.end() ){
            std::vector< TNode > args;
            Trace("sg-pat-debug") << "******* Get ground term for " << r << std::endl;
            Node n;
            if( getTermDatabase()->isInductionTerm( r ) ){
              n = d_op_arg_index[r].getGroundTerm( this, args );
            }else{
              n = r;
            }
            if( !n.isNull() ){
              Trace("sg-pat") << "Ground term for eqc " << r << " : " << std::endl;
              Trace("sg-pat") << "   " << n << std::endl;
              d_ground_eqc_map[r] = n;
              success = true;
            }else{
              Trace("sg-pat-debug") << "...could not find ground term." << std::endl;
            }
          }
        }
      }while( success );
      //also get ground terms
      d_ground_terms.clear();
      for( unsigned i=0; i<eqcs.size(); i++ ){
        TNode r = eqcs[i];
        d_op_arg_index[r].getGroundTerms( this, d_ground_terms );
      }
      Trace("sg-proc") << "...done determine ground EQC" << std::endl;

      //debug printing
      if( Trace.isOn("sg-gen-eqc") ){
        for( unsigned i=0; i<eqcs.size(); i++ ){
          TNode r = eqcs[i];
          //print out members
          bool firstTime = true;
          bool isFalse = areEqual( r, getTermDatabase()->d_false );
          eq::EqClassIterator eqc_i = eq::EqClassIterator( r, ee );
          while( !eqc_i.isFinished() ){
            TNode n = (*eqc_i);
            if( !n.getAttribute(NoMatchAttribute()) && ( n.getKind()!=EQUAL || isFalse ) ){
              if( firstTime ){
                Trace("sg-gen-eqc") << "e" << d_em[r] << " : { " << std::endl;
                firstTime = false;
              }
              if( n.hasOperator() ){
                Trace("sg-gen-eqc") << "   (" << n.getOperator();
                getTermDatabase()->computeArgReps( n );
                for( unsigned i=0; i<getTermDatabase()->d_arg_reps[n].size(); i++ ){
                  Trace("sg-gen-eqc") << " e" << d_em[getTermDatabase()->d_arg_reps[n][i]];
                }
                Trace("sg-gen-eqc") << ") :: " << n << std::endl;
              }else{
                Trace("sg-gen-eqc") << "   " << n << std::endl;
              }
            }
            ++eqc_i;
          }
          if( !firstTime ){
            Trace("sg-gen-eqc") << "}" << std::endl;
            //print out ground term
            std::map< TNode, Node >::iterator it = d_ground_eqc_map.find( r );
            if( it!=d_ground_eqc_map.end() ){
              Trace("sg-gen-eqc") << "- Ground term : " << it->second << std::endl;
            }
          }
        }
      }

      Trace("sg-proc") << "Compute relevant eqc..." << std::endl;
      d_relevant_eqc[0].clear();
      d_relevant_eqc[1].clear();
      for( unsigned i=0; i<eqcs.size(); i++ ){
        TNode r = eqcs[i];
        std::map< TNode, Node >::iterator it = d_ground_eqc_map.find( r );
        unsigned index = 1;
        if( it==d_ground_eqc_map.end() ){
          index = 0;
        }
        //based on unproven conjectures? TODO
        d_relevant_eqc[index].push_back( r );
      }
      Trace("sg-gen-tg-debug") << "Initial relevant eqc : ";
      for( unsigned i=0; i<d_relevant_eqc[0].size(); i++ ){
        Trace("sg-gen-tg-debug") << "e" << d_em[d_relevant_eqc[0][i]] << " ";
      }
      Trace("sg-gen-tg-debug") << std::endl;
      Trace("sg-proc") << "...done compute relevant eqc" << std::endl;


      Trace("sg-proc") << "Collect signature information..." << std::endl;
      d_funcs.clear();
      d_typ_funcs.clear();
      d_func_kind.clear();
      d_func_args.clear();
      TypeNode tnull;
      for( std::map< Node, TermArgTrie >::iterator it = getTermDatabase()->d_func_map_trie.begin(); it != getTermDatabase()->d_func_map_trie.end(); ++it ){
        if( !getTermDatabase()->d_op_map[it->first].empty() ){
          Node nn = getTermDatabase()->d_op_map[it->first][0];
          if( isHandledTerm( nn ) && nn.getKind()!=APPLY_SELECTOR_TOTAL && !nn.getType().isBoolean() ){
            d_funcs.push_back( it->first );
            d_typ_funcs[tnull].push_back( it->first );
            d_typ_funcs[nn.getType()].push_back( it->first );
            for( unsigned i=0; i<nn.getNumChildren(); i++ ){
              d_func_args[it->first].push_back( nn[i].getType() );
            }
            d_func_kind[it->first] = nn.getKind();
            Trace("sg-rel-sig") << "Will enumerate function applications of : " << it->first << ", #args = " << d_func_args[it->first].size() << ", kind = " << nn.getKind() << std::endl;
            getTermDatabase()->computeUfEqcTerms( it->first );
          }
        }
      }
      //shuffle functions
      for( std::map< TypeNode, std::vector< TNode > >::iterator it = d_typ_funcs.begin(); it !=d_typ_funcs.end(); ++it ){
        std::random_shuffle( it->second.begin(), it->second.end() );
        if( it->first.isNull() ){
          Trace("sg-gen-tg-debug") << "In this order : ";
          for( unsigned i=0; i<it->second.size(); i++ ){
            Trace("sg-gen-tg-debug") << it->second[i] << " ";            
          }
          Trace("sg-gen-tg-debug") << std::endl;
        }
      }
      Trace("sg-proc") << "...done collect signature information" << std::endl;


      Trace("sg-proc") << "Build theorem index..." << std::endl;
      d_ue_canon.clear();
      d_thm_index.clear();
      std::vector< Node > provenConj;
      quantifiers::FirstOrderModel* m = d_quantEngine->getModel();
      for( int i=0; i<m->getNumAssertedQuantifiers(); i++ ){
        Node q = m->getAssertedQuantifier( i );
        Trace("sg-conjecture-debug") << "Is " << q << " a relevant theorem?" << std::endl;
        Node conjEq;
        if( q[1].getKind()==EQUAL ){
          bool isSubsume = false;
          bool inEe = false;
          for( unsigned r=0; r<2; r++ ){
            TNode nl = q[1][r==0 ? 0 : 1];
            TNode nr = q[1][r==0 ? 1 : 0];
            Node eq = nl.eqNode( nr );
            if( r==1 || std::find( d_conjectures.begin(), d_conjectures.end(), q )==d_conjectures.end() ){
              //must make it canonical
              std::map< TypeNode, unsigned > var_count;
              std::map< TNode, TNode > subs;
              Trace("sg-proc-debug") << "get canonical " << eq << std::endl;
              eq = getCanonicalTerm( eq, var_count, subs );
            }
            if( !eq.isNull() ){
              if( r==0 ){
                inEe = d_ee_conjectures.find( q[1] )!=d_ee_conjectures.end();
                if( !inEe ){
                  //add to universal equality engine
                  Node nl = getUniversalRepresentative( eq[0], true );
                  Node nr = getUniversalRepresentative( eq[1], true );
                  if( areUniversalEqual( nl, nr ) ){
                    isSubsume = true;
                    //set inactive (will be ignored by other modules)
                    d_quantEngine->getModel()->setQuantifierActive( q, false );
                  }else{
                    Node exp;
                    d_ee_conjectures[q[1]] = true;
                    d_uequalityEngine.assertEquality( nl.eqNode( nr ), true, exp );
                  }
                }
                Trace("sg-conjecture") << "*** CONJECTURE : currently proven" << (isSubsume ? " and subsumed" : "");
                Trace("sg-conjecture") << " : " << q[1] << std::endl;
                provenConj.push_back( q );
              }
              if( !isSubsume ){
                Trace("thm-db-debug") << "Adding theorem to database " << eq[0] << " == " << eq[1] << std::endl;
                d_thm_index.addTheorem( eq[0], eq[1] );
              }else{
                break;
              }
            }else{
              break;
            }
          }
        }
      }
      //examine status of other conjectures
      for( unsigned i=0; i<d_conjectures.size(); i++ ){
        Node q = d_conjectures[i];
        if( std::find( provenConj.begin(), provenConj.end(), q )==provenConj.end() ){
          //check each skolem variable
          bool disproven = true;
          std::vector< Node > sk;
          getTermDatabase()->getSkolemConstants( q, sk, true );
          Trace("sg-conjecture") << "    CONJECTURE : ";
          std::vector< Node > ce;
          for( unsigned j=0; j<sk.size(); j++ ){
            TNode k = sk[j];
            TNode rk = getRepresentative( k );
            std::map< TNode, Node >::iterator git = d_ground_eqc_map.find( rk );
            //check if it is a ground term
            if( git==d_ground_eqc_map.end() ){
              Trace("sg-conjecture") << "ACTIVE : " << q;
              if( Trace.isOn("sg-gen-eqc") ){
                Trace("sg-conjecture") << " { ";
                for( unsigned k=0; k<sk.size(); k++ ){ Trace("sg-conjecture") << sk[k] << ( j==k ? "*" : "" ) << " "; }
                Trace("sg-conjecture") << "}";
              }
              Trace("sg-conjecture") << std::endl;
              disproven = false;
              break;
            }else{
              ce.push_back( git->second );
            }
          }
          if( disproven ){
            Trace("sg-conjecture") << "disproven : " << q << " : ";
            for( unsigned i=0; i<ce.size(); i++ ){
              Trace("sg-conjecture") << q[0][i] << " -> " << ce[i] << " ";
            }
            Trace("sg-conjecture") << std::endl;
          }
        }
      }
      Trace("thm-db") << "Theorem database is : " << std::endl;
      d_thm_index.debugPrint( "thm-db" );
      Trace("thm-db") << std::endl;
      Trace("sg-proc") << "...done build theorem index" << std::endl;

      //clear patterns
      d_patterns.clear();
      d_pattern_var_id.clear();
      d_pattern_var_duplicate.clear();
      d_pattern_is_normal.clear();
      d_pattern_fun_id.clear();
      d_pattern_fun_sum.clear();
      d_rel_patterns.clear();
      d_rel_pattern_var_sum.clear();
      d_rel_pattern_typ_index.clear();
      d_rel_pattern_subs_index.clear();
      d_gen_lat_maximal.clear();
      d_gen_lat_child.clear();
      d_gen_lat_parent.clear();
      d_gen_depth.clear();

      //the following generates a set of relevant terms
      d_use_ccand_eqc = true;
      for( unsigned i=0; i<2; i++ ){
        d_ccand_eqc[i].clear();
        d_ccand_eqc[i].push_back( d_relevant_eqc[i] );
      }
      d_rel_term_count = 0;
      //consider all functions
      d_typ_tg_funcs.clear();
      for( std::map< TypeNode, std::vector< TNode > >::iterator it = d_typ_funcs.begin(); it != d_typ_funcs.end(); ++it ){
        d_typ_tg_funcs[it->first].insert( d_typ_tg_funcs[it->first].end(), it->second.begin(), it->second.end() );
      }
      std::map< TypeNode, unsigned > rt_var_max;
      std::vector< TypeNode > rt_types;
      //map from generalization depth to maximum depth
      //std::map< unsigned, unsigned > gdepth_to_tdepth;
      for( unsigned depth=1; depth<3; depth++ ){
        Assert( d_tg_alloc.empty() );
        Trace("sg-proc") << "Generate terms at depth " << depth << "..." << std::endl;
        Trace("sg-rel-term") << "Relevant terms of depth " << depth << " : " << std::endl;
        //set up environment
        d_var_id.clear();
        d_var_limit.clear();
        d_tg_id = 0;
        d_tg_gdepth = 0;
        d_tg_gdepth_limit = -1;
        //consider all types
        d_tg_alloc[0].reset( this, TypeNode::null() );
        while( d_tg_alloc[0].getNextTerm( this, depth ) ){
          Assert( d_tg_alloc[0].getGeneralizationDepth( this )==d_tg_gdepth );
          if( d_tg_alloc[0].getDepth( this )==depth ){
            //construct term
            Node nn = d_tg_alloc[0].getTerm( this );
            if( getUniversalRepresentative( nn )==nn ){
              d_rel_term_count++;
              Trace("sg-rel-term") << "*** Relevant term : ";
              d_tg_alloc[0].debugPrint( this, "sg-rel-term", "sg-rel-term-debug2" );
              Trace("sg-rel-term") << std::endl;

              for( unsigned r=0; r<2; r++ ){
                Trace("sg-gen-tg-eqc") << "...from equivalence classes (" << r << ") : ";
                int index = d_ccand_eqc[r].size()-1;
                for( unsigned j=0; j<d_ccand_eqc[r][index].size(); j++ ){
                  Trace("sg-gen-tg-eqc") << "e" << d_em[d_ccand_eqc[r][index][j]] << " ";
                }
                Trace("sg-gen-tg-eqc") << std::endl;
              }
              TypeNode tnn = nn.getType();
              Trace("sg-gen-tg-debug") << "...term is " << nn << std::endl;
              Assert( getUniversalRepresentative( nn )==nn );

              //add information about pattern
              Trace("sg-gen-tg-debug") << "Collect pattern information..." << std::endl;
              Assert( std::find( d_rel_patterns[tnn].begin(), d_rel_patterns[tnn].end(), nn )==d_rel_patterns[tnn].end() );
              d_rel_patterns[tnn].push_back( nn );
              //build information concerning the variables in this pattern
              unsigned sum = 0;
              std::map< TypeNode, unsigned > typ_to_subs_index;
              std::vector< TNode > gsubs_vars;
              for( std::map< TypeNode, unsigned >::iterator it = d_var_id.begin(); it != d_var_id.end(); ++it ){
                if( it->second>0 ){
                  typ_to_subs_index[it->first] = sum;
                  sum += it->second;
                  for( unsigned i=0; i<it->second; i++ ){
                    gsubs_vars.push_back( getFreeVar( it->first, i ) );
                  }
                }
              }
              d_rel_pattern_var_sum[nn] = sum;
              //register the pattern
              registerPattern( nn, tnn );
              Assert( d_pattern_is_normal[nn] );
              Trace("sg-gen-tg-debug") << "...done collect pattern information" << std::endl;

              //compute generalization relation
              Trace("sg-gen-tg-debug") << "Build generalization information..." << std::endl;
              std::map< TNode, bool > patProc;
              int maxGenDepth = -1;
              unsigned i = d_rel_patterns[tnn].size()-1;
              for( int j=(int)(i-1); j>=0; j-- ){
                TNode np = d_rel_patterns[tnn][j];
                if( patProc.find( np )==patProc.end() ){
                  Trace("sg-gen-tg-debug2") << "Check if generalized by " << np << "..." << std::endl;
                  if( isGeneralization( np, nn ) ){
                    Trace("sg-rel-terms-debug") << "  is generalized by : " << np << std::endl;
                    d_gen_lat_child[np].push_back( nn );
                    d_gen_lat_parent[nn].push_back( np );
                    if( d_gen_depth[np]>maxGenDepth ){
                      maxGenDepth = d_gen_depth[np];
                    }
                    //don't consider the transitive closure of generalizes
                    Trace("sg-gen-tg-debug2") << "Add generalizations" << std::endl;
                    addGeneralizationsOf( np, patProc );
                    Trace("sg-gen-tg-debug2") << "Done add generalizations" << std::endl;
                  }else{
                    Trace("sg-gen-tg-debug2") << "  is not generalized by : " << np << std::endl;
                  }
                }
              }
              if( d_gen_lat_parent[nn].empty() ){
                d_gen_lat_maximal[tnn].push_back( nn );
              }
              d_gen_depth[nn] = maxGenDepth+1;
              Trace("sg-rel-term-debug") << "  -> generalization depth is " << d_gen_depth[nn] << " <> " << depth << std::endl;
              Trace("sg-gen-tg-debug") << "...done build generalization information" << std::endl;

              //record information about types
              Trace("sg-gen-tg-debug") << "Collect type information..." << std::endl;
              PatternTypIndex * pti = &d_rel_pattern_typ_index;
              for( std::map< TypeNode, unsigned >::iterator it = d_var_id.begin(); it != d_var_id.end(); ++it ){
                pti = &pti->d_children[it->first][it->second];
                //record maximum
                if( rt_var_max.find( it->first )==rt_var_max.end() || it->second>rt_var_max[it->first] ){
                  rt_var_max[it->first] = it->second;
                }
              }
              if( std::find( rt_types.begin(), rt_types.end(), tnn )==rt_types.end() ){
                rt_types.push_back( tnn );
              }
              pti->d_terms.push_back( nn );
              Trace("sg-gen-tg-debug") << "...done collect type information" << std::endl;

              Trace("sg-gen-tg-debug") << "Build substitutions for ground EQC..." << std::endl;
              std::vector< TNode > gsubs_terms;
              gsubs_terms.resize( gsubs_vars.size() );
              int index = d_ccand_eqc[1].size()-1;
              for( unsigned j=0; j<d_ccand_eqc[1][index].size(); j++ ){
                TNode r = d_ccand_eqc[1][index][j];
                Trace("sg-gen-tg-eqc") << "  Matches for e" << d_em[r] << ", which is ground term " << d_ground_eqc_map[r] << ":" << std::endl;
                std::map< TypeNode, std::map< unsigned, TNode > > subs;
                std::map< TNode, bool > rev_subs;
                //only get ground terms
                unsigned mode = optFilterConfirmationOnlyGround() ? 2 : 0;
                d_tg_alloc[0].resetMatching( this, r, mode );
                while( d_tg_alloc[0].getNextMatch( this, r, subs, rev_subs ) ){
                  //we will be building substitutions
                  bool firstTime = true;
                  for( std::map< TypeNode, std::map< unsigned, TNode > >::iterator it = subs.begin(); it != subs.end(); ++it ){
                    unsigned tindex = typ_to_subs_index[it->first];
                    for( std::map< unsigned, TNode >::iterator it2 = it->second.begin(); it2 != it->second.end(); ++it2 ){
                      if( !firstTime ){
                        Trace("sg-gen-tg-eqc") << ", ";
                      }else{
                        firstTime = false;
                        Trace("sg-gen-tg-eqc") << "    ";
                      }
                      Trace("sg-gen-tg-eqc") << it->first << ":x" << it2->first << " -> " << it2->second;
                      Assert( tindex+it2->first<gsubs_terms.size() );
                      gsubs_terms[tindex+it2->first] = it2->second;
                    }
                  }
                  Trace("sg-gen-tg-eqc") << std::endl;
                  d_rel_pattern_subs_index[nn].addSubstitution( r, gsubs_vars, gsubs_terms );
                }
              }
              Trace("sg-gen-tg-debug") << "...done build substitutions for ground EQC" << std::endl;
            }else{
              Trace("sg-gen-tg-debug") << "> not canonical : " << nn << std::endl;
            }
          }else{
            Trace("sg-gen-tg-debug") << "> produced term at previous depth : ";
            d_tg_alloc[0].debugPrint( this, "sg-gen-tg-debug", "sg-gen-tg-debug" );
            Trace("sg-gen-tg-debug") << std::endl;
          }
        }
        Trace("sg-proc") << "...done generate terms at depth " << depth << std::endl;
    
        
        //now generate right hand sides


      }
      Trace("sg-stats") << "--------> Total relevant patterns : " << d_rel_term_count << std::endl;

      Trace("sg-proc") << "Generate properties..." << std::endl;
      //set up environment
      d_use_ccand_eqc = false;
      d_var_id.clear();
      d_var_limit.clear();
      for( std::map< TypeNode, unsigned >::iterator it = rt_var_max.begin(); it != rt_var_max.end(); ++it ){
        d_var_id[ it->first ] = it->second;
        d_var_limit[ it->first ] = it->second;
      }
      //set up environment for candidate conjectures
      d_cconj_at_depth.clear();
      for( unsigned i=0; i<2; i++ ){
        d_cconj[i].clear();
      }
      d_cconj_rhs_paired.clear();
      unsigned totalCount = 0;
      for( unsigned depth=0; depth<5; depth++ ){
        //consider types from relevant terms
        std::random_shuffle( rt_types.begin(), rt_types.end() );
        for( unsigned i=0; i<rt_types.size(); i++ ){
          Assert( d_tg_alloc.empty() );
          Trace("sg-proc") << "Generate relevant RHS terms of type " << rt_types[i] << " at depth " << depth << "..." << std::endl;
          d_tg_id = 0;
          d_tg_alloc[0].reset( this, rt_types[i] );
          while( d_tg_alloc[0].getNextTerm( this, depth ) && totalCount<100 ){
            if( d_tg_alloc[0].getDepth( this )==depth ){
              Node rhs = d_tg_alloc[0].getTerm( this );
              Trace("sg-rel-prop") << "Relevant RHS : " << rhs << std::endl;
              //register pattern
              Assert( rhs.getType()==rt_types[i] );
              registerPattern( rhs, rt_types[i] );
              //for each maximal node of type rt_types[i] in generalization lattice
              for( unsigned j=0; j<d_gen_lat_maximal[rt_types[i]].size(); j++ ){
                //add candidate conjecture
                addCandidateConjecture( d_gen_lat_maximal[rt_types[i]][j], rhs, 0 );
              }
              totalCount++;
            }
          }
          //could have been partial, we must clear
          d_tg_alloc.clear();
        }
        Trace("sg-proc") << "Process candidate conjectures up to RHS term depth " << depth << "..." << std::endl;
        for( unsigned conj_depth=0; conj_depth<depth; conj_depth++ ){
          //process all conjectures waiting at depth
          unsigned sz = d_cconj_at_depth[conj_depth].size();
          for( int i=(int)(sz-1); i>=0; i-- ){
            processCandidateConjecture( d_cconj_at_depth[conj_depth][i], conj_depth );
          }
          Assert( d_cconj_at_depth[conj_depth].size()==sz );
          d_cconj_at_depth[conj_depth].clear();
        }
        Trace("sg-proc") << "...done process candidate conjectures at RHS term depth " << depth << std::endl;
      }
      Trace("sg-proc") << "...done generate properties" << std::endl;

      if( !d_waiting_conjectures.empty() ){
        Trace("sg-proc") << "Generated " << d_waiting_conjectures.size() << " conjectures." << std::endl;
        d_conjectures.insert( d_conjectures.end(), d_waiting_conjectures.begin(), d_waiting_conjectures.end() );
        for( unsigned i=0; i<d_waiting_conjectures.size(); i++ ){
          Assert( d_waiting_conjectures[i].getKind()==FORALL );
          Node lem = NodeManager::currentNM()->mkNode( OR, d_waiting_conjectures[i].negate(), d_waiting_conjectures[i] );
          d_quantEngine->getOutputChannel().lemma( lem );
          d_quantEngine->getOutputChannel().requirePhase( d_waiting_conjectures[i], false );
        }
        d_waiting_conjectures.clear();
      }

      Trace("thm-ee") << "Universal equality engine is : " << std::endl;
      eq::EqClassesIterator ueqcs_i = eq::EqClassesIterator( &d_uequalityEngine );
      while( !ueqcs_i.isFinished() ){
        TNode r = (*ueqcs_i);
        bool firstTime = true;
        TNode rr = getUniversalRepresentative( r );
        Trace("thm-ee") << "  " << r;
        if( rr!=r ){ Trace("thm-ee") << " [" << rr << "]"; }
        Trace("thm-ee") << " : { ";
        eq::EqClassIterator ueqc_i = eq::EqClassIterator( r, &d_uequalityEngine );
        while( !ueqc_i.isFinished() ){
          TNode n = (*ueqc_i);
          if( r!=n ){
            if( firstTime ){
              Trace("thm-ee") << std::endl;
              firstTime = false;
            }
            Trace("thm-ee") << "    " << n << std::endl;
          }
          ++ueqc_i;
        }
        if( !firstTime ){ Trace("thm-ee") << "  "; }
        Trace("thm-ee") << "}" << std::endl;
        ++ueqcs_i;
      }
      Trace("thm-ee") << std::endl;
    }
  }
}

void ConjectureGenerator::registerQuantifier( Node q ) {

}

void ConjectureGenerator::assertNode( Node n ) {

}


unsigned ConjectureGenerator::getNumTgVars( TypeNode tn ) {
  //return d_var_tg.size();
  return d_var_id[tn];
}

bool ConjectureGenerator::allowVar( TypeNode tn ) {
  std::map< TypeNode, unsigned >::iterator it = d_var_limit.find( tn );
  if( it==d_var_limit.end() ){
    return true;
  }else{
    return d_var_id[tn]<it->second;
  }
}

void ConjectureGenerator::addVar( TypeNode tn ) {
  //d_var_tg.push_back( v );
  d_var_id[tn]++;
  //d_var_eq_tg.push_back( std::vector< unsigned >() );
}

void ConjectureGenerator::removeVar( TypeNode tn ) {
  d_var_id[tn]--;
  //d_var_eq_tg.pop_back();
  //d_var_tg.pop_back();
}

unsigned ConjectureGenerator::getNumTgFuncs( TypeNode tn ) {
  return d_typ_tg_funcs[tn].size();
}

TNode ConjectureGenerator::getTgFunc( TypeNode tn, unsigned i ) {
  return d_typ_tg_funcs[tn][i];
}

bool ConjectureGenerator::considerCurrentTerm() {
  Assert( !d_tg_alloc.empty() );
  
  //if generalization depth is too large, don't consider it
  unsigned i = d_tg_alloc.size();
  Trace("sg-gen-tg-debug") << "Consider term ";
  d_tg_alloc[0].debugPrint( this, "sg-gen-tg-debug", "sg-gen-tg-debug" );
  Trace("sg-gen-tg-debug") << "?  curr term size = " << d_tg_alloc.size() << ", last status = " << d_tg_alloc[i-1].d_status;
  Trace("sg-gen-tg-debug") << std::endl;
  
  Assert( d_tg_alloc[0].getGeneralizationDepth( this )==d_tg_gdepth );
  
  if( d_tg_gdepth_limit>=0 && d_tg_gdepth>(unsigned)d_tg_gdepth_limit ){
    Trace("sg-gen-consider-term") << "-> generalization depth of ";
    d_tg_alloc[0].debugPrint( this, "sg-gen-consider-term", "sg-gen-tg-debug" );
    Trace("sg-gen-consider-term") << " is too high " << d_tg_gdepth << " " << d_tg_alloc[0].getGeneralizationDepth( this ) << ", do not consider." << std::endl;
    return false;
  }
  
  //----optimizations
  if( d_tg_alloc[i-1].d_status==1 ){
  }else if( d_tg_alloc[i-1].d_status==2 ){
  }else if( d_tg_alloc[i-1].d_status==5 ){
  }else{
    Trace("sg-gen-tg-debug") << "Bad tg: " << &d_tg_alloc[i-1] << std::endl;
    Assert( false );
  }
  //if equated two variables, first check if context-independent TODO
  //----end optimizations


  //check based on which candidate equivalence classes match
  if( d_use_ccand_eqc ){
    Trace("sg-gen-tg-debug") << "Filter based on relevant ground EQC";
    Trace("sg-gen-tg-debug") << ", #eqc to try = " << d_ccand_eqc[0][i-1].size() << "/" << d_ccand_eqc[1][i-1].size() << std::endl;

    Assert( d_ccand_eqc[0].size()>=2 );
    Assert( d_ccand_eqc[0].size()==d_ccand_eqc[1].size() );
    Assert( d_ccand_eqc[0].size()==d_tg_id+1 );
    Assert( d_tg_id==d_tg_alloc.size() );
    for( unsigned r=0; r<2; r++ ){
      d_ccand_eqc[r][i].clear();
    }

    //re-check feasibility of EQC
    for( unsigned r=0; r<2; r++ ){
      for( unsigned j=0; j<d_ccand_eqc[r][i-1].size(); j++ ){
        std::map< TypeNode, std::map< unsigned, TNode > > subs;
        std::map< TNode, bool > rev_subs;
        unsigned mode;
        if( r==0 ){
          mode = optReqDistinctVarPatterns() ? 1 : 0;
        }else{
          mode = (optFilterConfirmation() && optFilterConfirmationOnlyGround() ) ? 2 : 0;
        }
        d_tg_alloc[0].resetMatching( this, d_ccand_eqc[r][i-1][j], mode );
        if( d_tg_alloc[0].getNextMatch( this, d_ccand_eqc[r][i-1][j], subs, rev_subs ) ){
          d_ccand_eqc[r][i].push_back( d_ccand_eqc[r][i-1][j] );
        }
      }
    }
    for( unsigned r=0; r<2; r++ ){
      Trace("sg-gen-tg-debug") << "Current eqc of type " << r << " : ";
      for( unsigned j=0; j<d_ccand_eqc[r][i].size(); j++ ){
        Trace("sg-gen-tg-debug") << "e" << d_em[d_ccand_eqc[r][i][j]] << " ";
      }
      Trace("sg-gen-tg-debug") << std::endl;
    }
    if( d_ccand_eqc[0][i].empty() ){
      Trace("sg-gen-consider-term") << "Do not consider term of form ";
      d_tg_alloc[0].debugPrint( this, "sg-gen-consider-term", "sg-gen-consider-term-debug" );
      Trace("sg-gen-consider-term") << " since no relevant EQC matches it." << std::endl;
      return false;
    }
    if( d_ccand_eqc[1][i].empty() && optFilterConfirmation() ){
      Trace("sg-gen-consider-term") << "Do not consider term of form ";
      d_tg_alloc[0].debugPrint( this, "sg-gen-consider-term", "sg-gen-consider-term-debug" );
      Trace("sg-gen-consider-term") << " since no ground EQC matches it." << std::endl;
      return false;
    }
  }
  Trace("sg-gen-tg-debug") << "Will consider term ";
  d_tg_alloc[0].debugPrint( this, "sg-gen-tg-debug", "sg-gen-tg-debug" );
  Trace("sg-gen-tg-debug") << std::endl;
  Trace("sg-gen-consider-term-debug") << std::endl;
  return true;
}

bool ConjectureGenerator::considerTermCanon( unsigned tg_id ){
  Assert( tg_id<d_tg_alloc.size() );
  //check based on a canonicity of the term (if there is one)
  Trace("sg-gen-tg-debug") << "Consider term canon ";
  d_tg_alloc[0].debugPrint( this, "sg-gen-tg-debug", "sg-gen-tg-debug" );
  Trace("sg-gen-tg-debug") << ", tg is [" << tg_id << "]..." << std::endl;

  Node ln = d_tg_alloc[tg_id].getTerm( this );
  Trace("sg-gen-tg-debug") << "Term is " << ln << std::endl;
  if( !ln.isNull() ){
    //do not consider if it is non-canonical, and either:
    //  (1) we are not filtering based on matching candidate eqc, or
    //  (2) its canonical form is a generalization.
    TNode lnr = getUniversalRepresentative( ln, true );
    if( lnr==ln ){
      markReportedCanon( ln );
    }else if( !d_use_ccand_eqc || isGeneralization( lnr, ln ) ){
      Trace("sg-gen-consider-term") << "Do not consider term of form ";
      d_tg_alloc[0].debugPrint( this, "sg-gen-consider-term", "sg-gen-consider-term-debug" );
      Trace("sg-gen-consider-term") << " since sub-term " << ln << " is not canonical representation (which is " << lnr << ")." << std::endl;
      return false;
    }
  }
  Trace("sg-gen-tg-debug") << "Will consider term canon ";
  d_tg_alloc[0].debugPrint( this, "sg-gen-tg-debug", "sg-gen-tg-debug" );
  Trace("sg-gen-tg-debug") << std::endl;
  Trace("sg-gen-consider-term-debug") << std::endl;
  return true;
}

void ConjectureGenerator::changeContext( bool add ) {
  if( add ){
    for( unsigned r=0; r<2; r++ ){
      d_ccand_eqc[r].push_back( std::vector< TNode >() );
    }
    d_tg_id++;
  }else{
    for( unsigned r=0; r<2; r++ ){
      d_ccand_eqc[r].pop_back();
    }
    d_tg_id--;
    Assert( d_tg_alloc.find( d_tg_id )!=d_tg_alloc.end() );
    d_tg_alloc.erase( d_tg_id );
  }
}

unsigned ConjectureGenerator::collectFunctions( TNode opat, TNode pat, std::map< TNode, unsigned >& funcs,
                                             std::map< TypeNode, unsigned >& mnvn, std::map< TypeNode, unsigned >& mxvn ){
  if( pat.hasOperator() ){
    funcs[pat.getOperator()]++;
    unsigned sum = 1;
    for( unsigned i=0; i<pat.getNumChildren(); i++ ){
      sum += collectFunctions( opat, pat[i], funcs, mnvn, mxvn );
    }
    return sum;
  }else{
    Assert( pat.getNumChildren()==0 );
    funcs[pat]++;
    //for variables
    if( pat.getKind()==BOUND_VARIABLE ){
      if( funcs[pat]>1 ){
        //duplicate variable
        d_pattern_var_duplicate[opat]++;
      }else{
        //check for max/min
        TypeNode tn = pat.getType();
        unsigned vn = d_free_var_num[pat];
        std::map< TypeNode, unsigned >::iterator it = mnvn.find( tn );
        if( it!=mnvn.end() ){
          if( vn<it->second ){
            d_pattern_is_normal[opat] = false;
            mnvn[tn] = vn;
          }else if( vn>mxvn[tn] ){
            if( vn!=mxvn[tn]+1 ){
              d_pattern_is_normal[opat] = false;
            }
            mxvn[tn] = vn;
          }
        }else{
          //first variable of this type
          mnvn[tn] = vn;
          mxvn[tn] = vn;
        }
      }
    }
    return 1;
  }
}

void ConjectureGenerator::registerPattern( Node pat, TypeNode tpat ) {
  if( std::find( d_patterns[tpat].begin(), d_patterns[tpat].end(), pat )==d_patterns[tpat].end() ){
    d_patterns[TypeNode::null()].push_back( pat );
    d_patterns[tpat].push_back( pat );

    Assert( d_pattern_fun_id.find( pat )==d_pattern_fun_id.end() );
    Assert( d_pattern_var_id.find( pat )==d_pattern_var_id.end() );

    //collect functions
    std::map< TypeNode, unsigned > mnvn;
    d_pattern_fun_sum[pat] = collectFunctions( pat, pat, d_pattern_fun_id[pat], mnvn, d_pattern_var_id[pat] );
    if( d_pattern_is_normal.find( pat )==d_pattern_is_normal.end() ){
      d_pattern_is_normal[pat] = true;
    }
  }
}

bool ConjectureGenerator::isGeneralization( TNode patg, TNode pat, std::map< TNode, TNode >& subs ) {
  if( patg.getKind()==BOUND_VARIABLE ){
    std::map< TNode, TNode >::iterator it = subs.find( patg );
    if( it!=subs.end() ){
      return it->second==pat;
    }else{
      subs[patg] = pat;
      return true;
    }
  }else{
    Assert( patg.hasOperator() );
    if( !pat.hasOperator() || patg.getOperator()!=pat.getOperator() ){
      return false;
    }else{
      Assert( patg.getNumChildren()==pat.getNumChildren() );
      for( unsigned i=0; i<patg.getNumChildren(); i++ ){
        if( !isGeneralization( patg[i], pat[i], subs ) ){
          return false;
        }
      }
      return true;
    }
  }
}

void ConjectureGenerator::addGeneralizationsOf( TNode pat, std::map< TNode, bool >& patProc ) {
  patProc[pat] = true;
  for( unsigned k=0; k<d_gen_lat_parent[pat].size(); k++ ){
    addGeneralizationsOf( d_gen_lat_parent[pat][k], patProc );
  }
}

void ConjectureGenerator::addCandidateConjecture( TNode lhs, TNode rhs, unsigned depth ) {
  if( std::find( d_cconj_rhs_paired[rhs].begin(), d_cconj_rhs_paired[rhs].end(), lhs )==d_cconj_rhs_paired[rhs].end() ){
    //add conjecture to list to process
    d_cconj_at_depth[depth].push_back( d_cconj[0].size() );
    //define conjecture
    d_cconj[0].push_back( lhs );
    d_cconj[1].push_back( rhs );
    d_cconj_rhs_paired[rhs].push_back( lhs );
  }
}

void ConjectureGenerator::processCandidateConjecture( unsigned cid, unsigned depth ) {
  if( d_waiting_conjectures.size()>=optFullCheckConjectures() ){
    return;
  }
  TNode lhs = d_cconj[0][cid];
  TNode rhs = d_cconj[1][cid];
  if( !considerCandidateConjecture( lhs, rhs ) ){
    //push to children of generalization lattice
    for( unsigned i=0; i<d_gen_lat_child[lhs].size(); i++ ){
      if( d_gen_depth[lhs]+1==d_gen_depth[d_gen_lat_child[lhs][i]] ){
        addCandidateConjecture( d_gen_lat_child[lhs][i], rhs, depth+1 );
      }
    }
  }else{
    Trace("sg-conjecture") << "* Candidate conjecture : " << lhs << " == " << rhs << std::endl;
    Trace("sg-conjecture-debug") << "     LHS generalization depth : " << d_gen_depth[lhs] << std::endl;
    if( optFilterConfirmation() || optFilterFalsification() ){
      Trace("sg-conjecture-debug") << "     confirmed = " << d_subs_confirmCount << ", #witnesses range = " << d_subs_confirmWitnessRange.size() << "." << std::endl;
      Trace("sg-conjecture-debug") << "     #witnesses for ";
      bool firstTime = true;
      for( std::map< TNode, std::vector< TNode > >::iterator it = d_subs_confirmWitnessDomain.begin(); it != d_subs_confirmWitnessDomain.end(); ++it ){
        if( !firstTime ){
          Trace("sg-conjecture-debug") << ", ";
        }
        Trace("sg-conjecture-debug") << it->first << " : " << it->second.size() << "/" << d_pattern_fun_id[lhs][it->first];
        firstTime = false;
      }
      Trace("sg-conjecture-debug") << std::endl;
    }
    /*
    if( getUniversalRepresentative( lhs )!=lhs ){
      std::cout << "bad universal representative LHS : " << lhs << " " << getUniversalRepresentative( lhs ) << std::endl;
      exit(0);
    }
    if( getUniversalRepresentative( rhs )!=rhs ){
      std::cout << "bad universal representative RHS : " << rhs << " " << getUniversalRepresentative( rhs ) << std::endl;
      exit(0);
    }
    */
    Assert( getUniversalRepresentative( rhs )==rhs );
    Assert( getUniversalRepresentative( lhs )==lhs );
    //make universal quantified formula
    Assert( std::find( d_eq_conjectures[lhs].begin(), d_eq_conjectures[lhs].end(), rhs )==d_eq_conjectures[lhs].end() );
    d_eq_conjectures[lhs].push_back( rhs );
    d_eq_conjectures[rhs].push_back( lhs );
    std::vector< Node > bvs;
    for( std::map< TypeNode, unsigned >::iterator it = d_pattern_var_id[lhs].begin(); it != d_pattern_var_id[lhs].end(); ++it ){
      for( unsigned i=0; i<=it->second; i++ ){
        bvs.push_back( getFreeVar( it->first, i ) );
      }
    }
    Node bvl = NodeManager::currentNM()->mkNode( BOUND_VAR_LIST, bvs );
    Node conj = NodeManager::currentNM()->mkNode( FORALL, bvl, lhs.eqNode( rhs ) );
    conj = Rewriter::rewrite( conj );
    Trace("sg-conjecture-debug") << "   formula is : " << conj << std::endl;
    d_waiting_conjectures.push_back( conj );
  }
}

bool ConjectureGenerator::considerCandidateConjecture( TNode lhs, TNode rhs ) {
  Trace("sg-cconj-debug") << "Consider candidate conjecture : " << lhs << " == " << rhs << "?" << std::endl;
  if( lhs==rhs ){
    Trace("sg-cconj-debug") << "  -> trivial." << std::endl;
    return false;
  }else{
    if( lhs.getKind()==APPLY_CONSTRUCTOR && rhs.getKind()==APPLY_CONSTRUCTOR ){
      Trace("sg-cconj-debug") << "  -> irrelevant by syntactic analysis." << std::endl;
      return false;
    }
    //variables of LHS must subsume variables of RHS
    for( std::map< TypeNode, unsigned >::iterator it = d_pattern_var_id[rhs].begin(); it != d_pattern_var_id[rhs].end(); ++it ){
      std::map< TypeNode, unsigned >::iterator itl = d_pattern_var_id[lhs].find( it->first );
      if( itl!=d_pattern_var_id[lhs].end() ){
        if( itl->second<it->second ){
          Trace("sg-cconj-debug") << "  -> variables of sort " << it->first << " are not subsumed." << std::endl;
          return false;
        }else{
          Trace("sg-cconj-debug2") << "  variables of sort " << it->first << " are : " << itl->second << " vs " << it->second << std::endl;
        }
      }else{
        Trace("sg-cconj-debug") << "  -> has no variables of sort " << it->first << "." << std::endl;
        return false;
      }
    }
    //currently active conjecture?
    std::map< Node, std::vector< Node > >::iterator iteq = d_eq_conjectures.find( lhs );
    if( iteq!=d_eq_conjectures.end() ){
      if( std::find( iteq->second.begin(), iteq->second.end(), rhs )!=iteq->second.end() ){
        Trace("sg-cconj-debug") << "  -> already are considering this conjecture." << std::endl;
        return false;
      }
    }
    Trace("sg-cconj") << "Consider possible candidate conjecture : " << lhs << " == " << rhs << "?" << std::endl;
    //find witness for counterexample, if possible
    if( optFilterConfirmation() || optFilterFalsification() ){
      Assert( d_rel_pattern_var_sum.find( lhs )!=d_rel_pattern_var_sum.end() );
      Trace("sg-cconj-debug") << "Notify substitutions over " << d_rel_pattern_var_sum[lhs] << " variables." << std::endl;
      std::map< TNode, TNode > subs;
      d_subs_confirmCount = 0;
      d_subs_confirmWitnessRange.clear();
      d_subs_confirmWitnessDomain.clear();
      if( !d_rel_pattern_subs_index[lhs].notifySubstitutions( this, subs, rhs, d_rel_pattern_var_sum[lhs] ) ){
        Trace("sg-cconj") << "  -> found witness that falsifies the conjecture." << std::endl;
        return false;
      }
      if( optFilterConfirmation() ){
        if( d_subs_confirmCount==0 ){
          Trace("sg-cconj") << "  -> not confirmed by a ground substitutions." << std::endl;
          return false;
        }
      }
      if( optFilterConfirmationDomain() ){
        for( std::map< TNode, std::vector< TNode > >::iterator it = d_subs_confirmWitnessDomain.begin(); it != d_subs_confirmWitnessDomain.end(); ++it ){
          Assert( d_pattern_fun_id[lhs].find( it->first )!=d_pattern_fun_id[lhs].end() );
          unsigned req = d_pattern_fun_id[lhs][it->first];
          std::map< TNode, unsigned >::iterator itrf = d_pattern_fun_id[rhs].find( it->first );
          if( itrf!=d_pattern_fun_id[rhs].end() ){
            req = itrf->second>req ? itrf->second : req;
          }
          if( it->second.size()<req ){
            Trace("sg-cconj") << "  -> did not find at least " << d_pattern_fun_id[lhs][it->first] << " different values in ground substitutions for variable " << it->first << "." << std::endl;
            return false;
          }
        }
      }
    }

    Trace("sg-cconj") << "  -> SUCCESS." << std::endl;
    if( optFilterConfirmation() || optFilterFalsification() ){
      Trace("sg-cconj") << "     confirmed = " << d_subs_confirmCount << ", #witnesses range = " << d_subs_confirmWitnessRange.size() << "." << std::endl;
      for( std::map< TNode, std::vector< TNode > >::iterator it = d_subs_confirmWitnessDomain.begin(); it != d_subs_confirmWitnessDomain.end(); ++it ){
        Trace("sg-cconj") << "     #witnesses for " << it->first << " : " << it->second.size() << std::endl;
      }
    }

    return true;
  }
}



bool ConjectureGenerator::processCandidateConjecture2( TNode rhs, TypeNode tn, unsigned depth ) {
  for( unsigned j=0; j<d_rel_patterns_at_depth[tn][depth].size(); j++ ){
    if( processCandidateConjecture2( d_rel_patterns_at_depth[tn][depth][j], rhs ) ){
      return true;
    }
  }
  return false;
}

bool ConjectureGenerator::processCandidateConjecture2( TNode lhs, TNode rhs ) {
  if( !considerCandidateConjecture( lhs, rhs ) ){
    return false;
  }else{
    Trace("sg-conjecture") << "* Candidate conjecture : " << lhs << " == " << rhs << std::endl;
    Trace("sg-conjecture-debug") << "     LHS generalization depth : " << d_gen_depth[lhs] << std::endl;
    if( optFilterConfirmation() || optFilterFalsification() ){
      Trace("sg-conjecture-debug") << "     confirmed = " << d_subs_confirmCount << ", #witnesses range = " << d_subs_confirmWitnessRange.size() << "." << std::endl;
      Trace("sg-conjecture-debug") << "     #witnesses for ";
      bool firstTime = true;
      for( std::map< TNode, std::vector< TNode > >::iterator it = d_subs_confirmWitnessDomain.begin(); it != d_subs_confirmWitnessDomain.end(); ++it ){
        if( !firstTime ){
          Trace("sg-conjecture-debug") << ", ";
        }
        Trace("sg-conjecture-debug") << it->first << " : " << it->second.size() << "/" << d_pattern_fun_id[lhs][it->first];
        firstTime = false;
      }
      Trace("sg-conjecture-debug") << std::endl;
    }
    if( getUniversalRepresentative( lhs )!=lhs ){
      Trace("ajr-temp") << "bad universal representative : " << lhs << " " << getUniversalRepresentative( lhs ) << std::endl;
    }
    Assert( getUniversalRepresentative( rhs )==rhs );
    Assert( getUniversalRepresentative( lhs )==lhs );
    //make universal quantified formula
    Assert( std::find( d_eq_conjectures[lhs].begin(), d_eq_conjectures[lhs].end(), rhs )==d_eq_conjectures[lhs].end() );
    d_eq_conjectures[lhs].push_back( rhs );
    d_eq_conjectures[rhs].push_back( lhs );
    std::vector< Node > bvs;
    for( std::map< TypeNode, unsigned >::iterator it = d_pattern_var_id[lhs].begin(); it != d_pattern_var_id[lhs].end(); ++it ){
      for( unsigned i=0; i<=it->second; i++ ){
        bvs.push_back( getFreeVar( it->first, i ) );
      }
    }
    Node bvl = NodeManager::currentNM()->mkNode( BOUND_VAR_LIST, bvs );
    Node conj = NodeManager::currentNM()->mkNode( FORALL, bvl, lhs.eqNode( rhs ) );
    conj = Rewriter::rewrite( conj );
    Trace("sg-conjecture-debug") << "   formula is : " << conj << std::endl;
    d_waiting_conjectures.push_back( conj );
    return true;
  }
}






bool ConjectureGenerator::notifySubstitution( TNode glhs, std::map< TNode, TNode >& subs, TNode rhs ) {
  if( Trace.isOn("sg-cconj-debug") ){
    Trace("sg-cconj-debug") << "Ground eqc for LHS : " << glhs << ", based on substituion: " << std::endl;
    for( std::map< TNode, TNode >::iterator it = subs.begin(); it != subs.end(); ++it ){
      Assert( getRepresentative( it->second )==it->second );
      Trace("sg-cconj-debug") << "  " << it->first << " -> " << it->second << std::endl;
    }
  }
  Trace("sg-cconj-debug") << "Evaluate RHS : : " << rhs << std::endl;
  //get the representative of rhs with substitution subs
  TNode grhs = getTermDatabase()->evaluateTerm( rhs, subs, true );
  Trace("sg-cconj-debug") << "...done evaluating term, got : " << grhs << std::endl;
  if( !grhs.isNull() ){
    if( glhs!=grhs ){
      if( optFilterFalsification() ){
        Trace("sg-cconj-debug") << "Ground eqc for RHS : " << grhs << std::endl;
        //check based on ground terms
        std::map< TNode, Node >::iterator itl = d_ground_eqc_map.find( glhs );
        if( itl!=d_ground_eqc_map.end() ){
          std::map< TNode, Node >::iterator itr = d_ground_eqc_map.find( grhs );
          if( itr!=d_ground_eqc_map.end() ){
            Trace("sg-cconj-debug") << "We have ground terms " << itl->second << " and " << itr->second << "." << std::endl;
            if( itl->second.isConst() && itr->second.isConst() ){
              Trace("sg-cconj-debug") << "...disequal constants." << std::endl;
              Trace("sg-cconj-witness") << "  Witness of falsification : " << itl->second << " != " << itr->second << ", substutition is : " << std::endl;
              for( std::map< TNode, TNode >::iterator it = subs.begin(); it != subs.end(); ++it ){
                Trace("sg-cconj-witness") << "    " << it->first << " -> " << it->second << std::endl;
              }
              return false;
            }
          }
        }
      }
        /*
      if( getEqualityEngine()->areDisequal( glhs, grhs, false ) ){
        Trace("sg-cconj-debug") << "..." << glhs << " and " << grhs << " are disequal." << std::endl;
        return false;
      }else{
        Trace("sg-cconj-debug") << "..." << glhs << " and " << grhs << " are not disequal." << std::endl;
      }
      */
    }else{
      Trace("sg-cconj-witness") << "  Witnessed " << glhs << " == " << grhs << ", substutition is : " << std::endl;
      for( std::map< TNode, TNode >::iterator it = subs.begin(); it != subs.end(); ++it ){
        Trace("sg-cconj-witness") << "    " << it->first << " -> " << it->second << std::endl;
        if( std::find( d_subs_confirmWitnessDomain[it->first].begin(), d_subs_confirmWitnessDomain[it->first].end(), it->second )==d_subs_confirmWitnessDomain[it->first].end() ){
          d_subs_confirmWitnessDomain[it->first].push_back( it->second );
        }
      }
      d_subs_confirmCount++;
      if( std::find( d_subs_confirmWitnessRange.begin(), d_subs_confirmWitnessRange.end(), glhs )==d_subs_confirmWitnessRange.end() ){
        d_subs_confirmWitnessRange.push_back( glhs );
      }
      Trace("sg-cconj-debug") << "RHS is identical." << std::endl;
    }
  }else{
    Trace("sg-cconj-debug") << "(could not ground eqc for RHS)." << std::endl;
  }
  return true;
}


void TermGenerator::reset( ConjectureGenerator * s, TypeNode tn ) {
  Assert( d_children.empty() );
  d_typ = tn;
  d_status = 0;
  d_status_num = 0;
  d_children.clear();
  Trace("sg-gen-tg-debug2") << "...add to context " << this << std::endl;
  d_id = s->d_tg_id;
  s->changeContext( true );
}

bool TermGenerator::getNextTerm( ConjectureGenerator * s, unsigned depth ) {
  if( Trace.isOn("sg-gen-tg-debug2") ){
    Trace("sg-gen-tg-debug2") << this << " getNextTerm depth " << depth << " : status = " << d_status << ", num = " << d_status_num;
    if( d_status==5 ){
      TNode f = s->getTgFunc( d_typ, d_status_num );
      Trace("sg-gen-tg-debug2") << ", f = " << f;
      Trace("sg-gen-tg-debug2") << ", #args = " << s->d_func_args[f].size();
      Trace("sg-gen-tg-debug2") << ", childNum = " << d_status_child_num;
      Trace("sg-gen-tg-debug2") << ", #children = " << d_children.size();
    }
    Trace("sg-gen-tg-debug2") << std::endl;
  }

  if( d_status==0 ){
    d_status++;
    if( !d_typ.isNull() ){
      if( s->allowVar( d_typ ) ){
        //allocate variable
        d_status_num = s->d_var_id[d_typ];
        s->addVar( d_typ );
        Trace("sg-gen-tg-debug2") << this << " ...return unique var #" << d_status_num << std::endl;
        return s->considerCurrentTerm() ? true : getNextTerm( s, depth );
      }else{
        //check allocating new variable
        d_status++;
        d_status_num = -1;
        s->d_tg_gdepth++;
        return getNextTerm( s, depth );
      }
    }else{
      d_status = 4;
      d_status_num = -1;
      return getNextTerm( s, depth );
    }
  }else if( d_status==2 ){
    //cleanup previous information
    //if( d_status_num>=0 ){
    //  s->d_var_eq_tg[d_status_num].pop_back();
    //}
    //check if there is another variable
    if( (d_status_num+1)<(int)s->getNumTgVars( d_typ ) ){
      d_status_num++;
      //we have equated two variables
      //s->d_var_eq_tg[d_status_num].push_back( d_id );
      Trace("sg-gen-tg-debug2") << this << "...consider other var #" << d_status_num << std::endl;
      return s->considerCurrentTerm() ? true : getNextTerm( s, depth );
    }else{
      s->d_tg_gdepth--;
      d_status++;
      return getNextTerm( s, depth );
    }
  }else if( d_status==4 ){
    d_status++;
    if( depth>0 && (d_status_num+1)<(int)s->getNumTgFuncs( d_typ ) ){
      d_status_num++;
      d_status_child_num = 0;
      Trace("sg-gen-tg-debug2") << this << "...consider function " << s->getTgFunc( d_typ, d_status_num ) << std::endl;
      s->d_tg_gdepth++;
      if( !s->considerCurrentTerm() ){
        s->d_tg_gdepth--;
        //don't consider this function
        d_status--;
      }else{
        //we have decided on a function application
      }
      return getNextTerm( s, depth );
    }else{
      //do not choose function applications at depth 0
      d_status++;
      return getNextTerm( s, depth );
    }
  }else if( d_status==5 ){
    //iterating over arguments
    TNode f = s->getTgFunc( d_typ, d_status_num );
    if( d_status_child_num<0 ){
      //no more arguments
      s->d_tg_gdepth--;
      d_status--;
      return getNextTerm( s, depth );
    }else if( d_status_child_num==(int)s->d_func_args[f].size() ){
      d_status_child_num--;
      return s->considerTermCanon( d_id ) ? true : getNextTerm( s, depth );
      //return true;
    }else{
      Assert( d_status_child_num<(int)s->d_func_args[f].size() );
      if( d_status_child_num==(int)d_children.size() ){
        d_children.push_back( s->d_tg_id );
        Assert( s->d_tg_alloc.find( s->d_tg_id )==s->d_tg_alloc.end() );
        s->d_tg_alloc[d_children[d_status_child_num]].reset( s, s->d_func_args[f][d_status_child_num] );
        return getNextTerm( s, depth );
      }else{
        Assert( d_status_child_num+1==(int)d_children.size() );
        if( s->d_tg_alloc[d_children[d_status_child_num]].getNextTerm( s, depth-1 ) ){
          d_status_child_num++;
          return getNextTerm( s, depth );
        }else{
          d_children.pop_back();
          d_status_child_num--;
          return getNextTerm( s, depth );
        }
      }
    }
  }else if( d_status==1 || d_status==3 ){
    if( d_status==1 ){
      s->removeVar( d_typ );
      Assert( d_status_num==(int)s->d_var_id[d_typ] );
      //check if there is only one feasible equivalence class.  if so, don't make pattern any more specific.
      //unsigned i = s->d_ccand_eqc[0].size()-1;
      //if( s->d_ccand_eqc[0][i].size()==1 && s->d_ccand_eqc[1][i].empty() ){
      //  Trace("ajr-temp") << "Apply this!" << std::endl;
      //  d_status = 6;
      //  return getNextTerm( s, depth );
      //}
      s->d_tg_gdepth++;
    }
    d_status++;
    d_status_num = -1;
    return getNextTerm( s, depth );
  }else{
    //clean up
    Assert( d_children.empty() );
    Trace("sg-gen-tg-debug2") << "...remove from context " << this << std::endl;
    s->changeContext( false );
    Assert( d_id==s->d_tg_id );
    return false;
  }
}

void TermGenerator::resetMatching( ConjectureGenerator * s, TNode eqc, unsigned mode ) {
  d_match_status = 0;
  d_match_status_child_num = 0;
  d_match_children.clear();
  d_match_children_end.clear();
  d_match_mode = mode;
}

bool TermGenerator::getNextMatch( ConjectureGenerator * s, TNode eqc, std::map< TypeNode, std::map< unsigned, TNode > >& subs, std::map< TNode, bool >& rev_subs ) {
  if( Trace.isOn("sg-gen-tg-match") ){
    Trace("sg-gen-tg-match") << "Matching ";
    debugPrint( s, "sg-gen-tg-match", "sg-gen-tg-match" );
    Trace("sg-gen-tg-match") << " with eqc e" << s->d_em[eqc] << "..." << std::endl;
    Trace("sg-gen-tg-match") << "   mstatus = " << d_match_status;
    if( d_status==5 ){
      TNode f = s->getTgFunc( d_typ, d_status_num );
      Trace("sg-gen-tg-debug2") << ", f = " << f;
      Trace("sg-gen-tg-debug2") << ", #args = " << s->d_func_args[f].size();
      Trace("sg-gen-tg-debug2") << ", mchildNum = " << d_match_status_child_num;
      Trace("sg-gen-tg-debug2") << ", #mchildren = " << d_match_children.size();
    }
    Trace("sg-gen-tg-debug2") << ", current substitution : {";
    for( std::map< TypeNode, std::map< unsigned, TNode > >::iterator itt = subs.begin(); itt != subs.end(); ++itt ){
      for( std::map< unsigned, TNode >::iterator it = itt->second.begin(); it != itt->second.end(); ++it ){
        Trace("sg-gen-tg-debug2")  << " " << it->first << " -> e" << s->d_em[it->second];
      }
    }
    Trace("sg-gen-tg-debug2") << " } " << std::endl;
  }
  if( d_status==1 ){
    //a variable
    if( d_match_status==0 ){
      d_match_status++;
      if( d_match_mode>=2 ){
        //only ground terms
        if( !s->isGroundEqc( eqc ) ){
          return false;
        }
      }
      if( d_match_mode%2==1 ){
        std::map< TNode, bool >::iterator it = rev_subs.find( eqc );
        if( it==rev_subs.end() ){
          rev_subs[eqc] = true;
        }else{
          return false;
        }
      }
      Assert( subs[d_typ].find( d_status_num )==subs[d_typ].end() );
      subs[d_typ][d_status_num] = eqc;
      return true;
    }else{
      //clean up
      subs[d_typ].erase( d_status_num );
      if( d_match_mode%2==1 ){
        rev_subs.erase( eqc );
      }
      return false;
    }
  }else if( d_status==2 ){
    if( d_match_status==0 ){
      d_match_status++;
      Assert( d_status_num<(int)s->getNumTgVars( d_typ ) );
      std::map< unsigned, TNode >::iterator it = subs[d_typ].find( d_status_num );
      Assert( it!=subs[d_typ].end() );
      return it->second==eqc;
    }else{
      return false;
    }
  }else if( d_status==5 ){
    //Assert( d_match_children.size()<=d_children.size() );
    //enumerating over f-applications in eqc
    if( d_match_status_child_num<0 ){
      return false;
    }else if( d_match_status==0 ){
      //set up next binding
      if( d_match_status_child_num==(int)d_match_children.size() ){
        if( d_match_status_child_num==0 ){
          //initial binding
          TNode f = s->getTgFunc( d_typ, d_status_num );
          std::map< TNode, TermArgTrie >::iterator it = s->getTermDatabase()->d_func_map_eqc_trie[f].d_data.find( eqc );
          if( it!=s->getTermDatabase()->d_func_map_eqc_trie[f].d_data.end() ){
            d_match_children.push_back( it->second.d_data.begin() );
            d_match_children_end.push_back( it->second.d_data.end() );
          }else{
            d_match_status++;
            d_match_status_child_num--;
            return getNextMatch( s, eqc, subs, rev_subs );
          }
        }else{
          d_match_children.push_back( d_match_children[d_match_status_child_num-1]->second.d_data.begin() );
          d_match_children_end.push_back( d_match_children[d_match_status_child_num-1]->second.d_data.end() );
        }
      }
      d_match_status++;
      Assert( d_match_status_child_num+1==(int)d_match_children.size() );
      if( d_match_children[d_match_status_child_num]==d_match_children_end[d_match_status_child_num] ){
        //no more arguments to bind
        d_match_children.pop_back();
        d_match_children_end.pop_back();
        d_match_status_child_num--;
        return getNextMatch( s, eqc, subs, rev_subs );
      }else{
        if( d_match_status_child_num==(int)d_children.size() ){
          //successfully matched all children
          d_match_children.pop_back();
          d_match_children_end.pop_back();
          d_match_status_child_num--;
          return true;//return d_match_children[d_match_status]!=d_match_children_end[d_match_status];
        }else{
          //do next binding
          s->d_tg_alloc[d_children[d_match_status_child_num]].resetMatching( s, d_match_children[d_match_status_child_num]->first, d_match_mode );
          return getNextMatch( s, eqc, subs, rev_subs );
        }
      }
    }else{
      Assert( d_match_status==1 );
      Assert( d_match_status_child_num+1==(int)d_match_children.size() );
      Assert( d_match_children[d_match_status_child_num]!=d_match_children_end[d_match_status_child_num] );
      d_match_status--;
      if( s->d_tg_alloc[d_children[d_match_status_child_num]].getNextMatch( s, d_match_children[d_match_status_child_num]->first, subs, rev_subs ) ){
        d_match_status_child_num++;
        return getNextMatch( s, eqc, subs, rev_subs );
      }else{
        //iterate
        d_match_children[d_match_status_child_num]++;
        return getNextMatch( s, eqc, subs, rev_subs );
      }
    }
  }
  Assert( false );
  return false;
}

unsigned TermGenerator::getDepth( ConjectureGenerator * s ) {
  if( d_status==5 ){
    unsigned maxd = 0;
    for( unsigned i=0; i<d_children.size(); i++ ){
      unsigned d = s->d_tg_alloc[d_children[i]].getDepth( s );
      if( d>maxd ){
        maxd = d;
      }
    }
    return 1+maxd;
  }else{
    return 0;
  }
}

unsigned TermGenerator::getGeneralizationDepth( ConjectureGenerator * s ) {
  if( d_status==5 ){
    unsigned sum = 1;
    for( unsigned i=0; i<d_children.size(); i++ ){
      sum += s->d_tg_alloc[d_children[i]].getGeneralizationDepth( s );
    }
    return sum;
  }else if( d_status==2 ){
    return 1;
  }else{
    Assert( d_status==1 );
    return 0;
  }
}

Node TermGenerator::getTerm( ConjectureGenerator * s ) {
  if( d_status==1 || d_status==2 ){
    Assert( !d_typ.isNull() );
    return s->getFreeVar( d_typ, d_status_num );
  }else if( d_status==5 ){
    Node f = s->getTgFunc( d_typ, d_status_num );
    if( d_children.size()==s->d_func_args[f].size() ){
      std::vector< Node > children;
      children.push_back( f );
      for( unsigned i=0; i<d_children.size(); i++ ){
        Node nc = s->d_tg_alloc[d_children[i]].getTerm( s );
        if( nc.isNull() ){
          return Node::null();
        }else{
          //Assert( nc.getType()==s->d_func_args[f][i] );
          children.push_back( nc );
        }
      }
      return NodeManager::currentNM()->mkNode( s->d_func_kind[f], children );
    }
  }else{
    Assert( false );
  }
  return Node::null();
}

/*
int TermGenerator::getActiveChild( ConjectureGenerator * s ) {
  if( d_status==1 || d_status==2 ){
    return d_id;
  }else if( d_status==5 ){
    Node f = s->getTgFunc( d_typ, d_status_num );
    int i = d_children.size()-1;
    if( d_children.size()==s->d_func_args[f].size() ){
      if( d_children.empty() ){
        return d_id;
      }else{
        int cac = s->d_tg_alloc[d_children[i]].getActiveChild( s );
        return cac==(int)d_children[i] ? d_id : cac;
      }
    }else if( !d_children.empty() ){
      return s->d_tg_alloc[d_children[i]].getActiveChild( s );
    }
  }else{
    Assert( false );
  }
  return -1;
}
*/

void TermGenerator::debugPrint( ConjectureGenerator * s, const char * c, const char * cd ) {
  Trace(cd) << "[*" << d_id << "," << d_status << "]:";
  if( d_status==1 || d_status==2 ){
    Trace(c) << s->getFreeVar( d_typ, d_status_num );
  }else if( d_status==5 ){
    TNode f = s->getTgFunc( d_typ, d_status_num );
    Trace(c) << "(" << f;
    for( unsigned i=0; i<d_children.size(); i++ ){
      Trace(c) << " ";
       s->d_tg_alloc[d_children[i]].debugPrint( s, c, cd );
    }
    if( d_children.size()<s->d_func_args[f].size() ){
      Trace(c) << " ...";
    }
    Trace(c) << ")";
  }else{
    Trace(c) << "???";
  }
}


void SubstitutionIndex::addSubstitution( TNode eqc, std::vector< TNode >& vars, std::vector< TNode >& terms, unsigned i ) {
  if( i==vars.size() ){
    d_var = eqc;
  }else{
    Assert( d_var.isNull() || d_var==vars[i] );
    d_var = vars[i];
    d_children[terms[i]].addSubstitution( eqc, vars, terms, i+1 );
  }
}

bool SubstitutionIndex::notifySubstitutions( ConjectureGenerator * s, std::map< TNode, TNode >& subs, TNode rhs, unsigned numVars, unsigned i ) {
  if( i==numVars ){
    Assert( d_children.empty() );
    return s->notifySubstitution( d_var, subs, rhs );
  }else{
    Assert( i==0 || !d_children.empty() );
    for( std::map< TNode, SubstitutionIndex >::iterator it = d_children.begin(); it != d_children.end(); ++it ){
      Trace("sg-cconj-debug2") << "Try " << d_var << " -> " << it->first << " (" << i << "/" << numVars << ")" << std::endl;
      subs[d_var] = it->first;
      if( !it->second.notifySubstitutions( s, subs, rhs, numVars, i+1 ) ){
        return false;
      }
    }
    return true;
  }
}


void TheoremIndex::addTheorem( std::vector< TNode >& lhs_v, std::vector< unsigned >& lhs_arg, TNode rhs ){
  if( lhs_v.empty() ){
    if( std::find( d_terms.begin(), d_terms.end(), rhs )==d_terms.end() ){
      d_terms.push_back( rhs );
    }
  }else{
    unsigned index = lhs_v.size()-1;
    if( lhs_arg[index]==lhs_v[index].getNumChildren() ){
      lhs_v.pop_back();
      lhs_arg.pop_back();
      addTheorem( lhs_v, lhs_arg, rhs );
    }else{
      lhs_arg[index]++;
      addTheoremNode( lhs_v[index][lhs_arg[index]-1], lhs_v, lhs_arg, rhs );
    }
  }
}

void TheoremIndex::addTheoremNode( TNode curr, std::vector< TNode >& lhs_v, std::vector< unsigned >& lhs_arg, TNode rhs ){
  Trace("thm-db-debug") << "Adding conjecture for subterm " << curr << "..." << std::endl;
  if( curr.hasOperator() ){
    lhs_v.push_back( curr );
    lhs_arg.push_back( 0 );
    d_children[curr.getOperator()].addTheorem( lhs_v, lhs_arg, rhs );
  }else{
    Assert( curr.getKind()==kind::BOUND_VARIABLE );
    Assert( d_var.isNull() || d_var==curr );
    d_var = curr;
    d_children[curr].addTheorem( lhs_v, lhs_arg, rhs );
  }
}

void TheoremIndex::getEquivalentTerms( std::vector< TNode >& n_v, std::vector< unsigned >& n_arg,
                                       std::map< TNode, TNode >& smap, std::vector< TNode >& vars, std::vector< TNode >& subs,
                                       std::vector< Node >& terms ) {
  Trace("thm-db-debug") << "Get equivalent terms " << n_v.size() << " " << n_arg.size() << std::endl;
  if( n_v.empty() ){
    Trace("thm-db-debug") << "Number of terms : " << d_terms.size() << std::endl;
    //apply substutitions to RHS's
    for( unsigned i=0; i<d_terms.size(); i++ ){
      Node n = d_terms[i].substitute( vars.begin(), vars.end(), subs.begin(), subs.end() );
      terms.push_back( n );
    }
  }else{
    unsigned index = n_v.size()-1;
    if( n_arg[index]==n_v[index].getNumChildren() ){
      n_v.pop_back();
      n_arg.pop_back();
      getEquivalentTerms( n_v, n_arg, smap, vars, subs, terms );
    }else{
      n_arg[index]++;
      getEquivalentTermsNode( n_v[index][n_arg[index]-1], n_v, n_arg, smap, vars, subs, terms );
    }
  }
}

void TheoremIndex::getEquivalentTermsNode( Node curr, std::vector< TNode >& n_v, std::vector< unsigned >& n_arg,
                                           std::map< TNode, TNode >& smap, std::vector< TNode >& vars, std::vector< TNode >& subs,
                                           std::vector< Node >& terms ) {
  Trace("thm-db-debug") << "Get equivalent based on subterm " << curr << "..." << std::endl;
  if( curr.hasOperator() ){
    Trace("thm-db-debug") << "Check based on operator..." << std::endl;
    std::map< TNode, TheoremIndex >::iterator it = d_children.find( curr.getOperator() );
    if( it!=d_children.end() ){
      n_v.push_back( curr );
      n_arg.push_back( 0 );
      it->second.getEquivalentTerms( n_v, n_arg, smap, vars, subs, terms );
    }
    Trace("thm-db-debug") << "...done check based on operator" << std::endl;
  }
  if( !d_var.isNull() ){
    Trace("thm-db-debug") << "Check for substitution with " << d_var << "..." << std::endl;
    if( curr.getType()==d_var.getType() ){
      //add to substitution if possible
      bool success = false;
      std::map< TNode, TNode >::iterator it = smap.find( d_var );
      if( it==smap.end() ){
        smap[d_var] = curr;
        vars.push_back( d_var );
        subs.push_back( curr );
        success = true;
      }else if( it->second==curr ){
        success = true;
      }else{
        //also check modulo equality (in universal equality engine)
      }
      Trace("thm-db-debug") << "...check for substitution with " << d_var << ", success = " << success << "." << std::endl;
      if( success ){
        d_children[d_var].getEquivalentTerms( n_v, n_arg, smap, vars, subs, terms );
      }
    }
  }
}

void TheoremIndex::debugPrint( const char * c, unsigned ind ) {
  for( std::map< TNode, TheoremIndex >::iterator it = d_children.begin(); it != d_children.end(); ++it ){
    for( unsigned i=0; i<ind; i++ ){ Trace(c) << "  "; }
    Trace(c) << it->first << std::endl;
    it->second.debugPrint( c, ind+1 );
  }
  if( !d_terms.empty() ){
    for( unsigned i=0; i<ind; i++ ){ Trace(c) << "  "; }
    Trace(c) << "{";
    for( unsigned i=0; i<d_terms.size(); i++ ){
      Trace(c) << " " << d_terms[i];
    }
    Trace(c) << " }" << std::endl;
  }
  //if( !d_var.isNull() ){
  //  for( unsigned i=0; i<ind; i++ ){ Trace(c) << "  "; }
  //  Trace(c) << "var:" << d_var << std::endl;
  //}
}

bool ConjectureGenerator::optReqDistinctVarPatterns() { return false; }
bool ConjectureGenerator::optFilterFalsification() { return true; }
bool ConjectureGenerator::optFilterConfirmation() { return true; }
bool ConjectureGenerator::optFilterConfirmationDomain() { return true; }
bool ConjectureGenerator::optFilterConfirmationOnlyGround() { return true; }//false; }
bool ConjectureGenerator::optWaitForFullCheck() { return true; }
unsigned ConjectureGenerator::optFullCheckFrequency() { return 1; }
unsigned ConjectureGenerator::optFullCheckConjectures() { return 1; }


}

