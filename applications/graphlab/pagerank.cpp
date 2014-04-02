////////////////////////////////////////////////////////////////////////
// This file is part of Grappa, a system for scaling irregular
// applications on commodity clusters. 

// Copyright (C) 2010-2014 University of Washington and Battelle
// Memorial Institute. University of Washington authorizes use of this
// Grappa software.

// Grappa is free software: you can redistribute it and/or modify it
// under the terms of the Affero General Public License as published
// by Affero, Inc., either version 1 of the License, or (at your
// option) any later version.

// Grappa is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// Affero General Public License for more details.

// You should have received a copy of the Affero General Public
// License along with this program. If not, you may obtain one from
// http://www.affero.org/oagl.html.
////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////
/// Demonstrates using the GraphLab API to implement Pagerank
////////////////////////////////////////////////////////////////////////
#include <Grappa.hpp>
#include "graphlab.hpp"

DEFINE_bool( metrics, false, "Dump metrics");

DEFINE_int32(scale, 10, "Log2 number of vertices.");
DEFINE_int32(edgefactor, 16, "Average number of edges per vertex.");


GRAPPA_DEFINE_METRIC(SimpleMetric<double>, construction_time, 0);

const double RESET_PROB = 0.15;
const double TOLERANCE = 1.0E-2;

Reducer<int64_t,ReducerType::Add> active_count;

struct PagerankVertexData : public GraphlabVertex<double> {

  double rank;
  
  PagerankVertexData(double initial_rank = 0.0)
    : GraphlabVertex()
    , rank(initial_rank)
  { }
};

using G = Graph<PagerankVertexData,Empty>;

struct PagerankVertexProgram {
  double last_change;
  
  bool gather_edges(const G::Vertex& v) const { return true; }
  bool scatter_edges(const G::Vertex& v) const { return true; }
  
  double gather(G::Vertex& src, G::Edge& e) const {
    return src->rank / src.nadj;
  }
  void apply(G::Vertex& v, double total) {
    auto new_val = (1.0 - RESET_PROB) * v->cache + RESET_PROB;
    last_change = (new_val - v->rank) / v.nadj;
    v->rank = new_val;
  }
  void scatter(const G::Edge& e, G::Vertex& target) const {
    target->post_delta(last_change);
    if (std::fabs(last_change) > TOLERANCE) {
      target->activate();
    }
  }
};

int main(int argc, char* argv[]) {
  init(&argc, &argv);
  run([]{
    int64_t NE = (1L << FLAGS_scale) * FLAGS_edgefactor;

    double t;    
    t = walltime();
    
    auto tg = TupleGraph::Kronecker(FLAGS_scale, NE, 111, 222);
    auto g = G::create(tg);
    
    // TODO: random init
    forall(g, [](G::Vertex& v){ new (&v.data) PagerankVertexData(0.2); });
    
    tg.destroy();
    
    construction_time = (walltime()-t);
    LOG(INFO) << construction_time;
    
    LOG(INFO) << "starting pagerank";

#ifndef MANUAL
    activate_all(g);
    run_synchronous< PagerankVertexProgram >(g);
#else
    // "gather" once to initialize cache (doing with a scatter)
    forall(g, [=](G::Vertex& v){
      auto delta = v->rank / v.nadj;
      forall<async>(adj(g,v), [=](G::Edge& e){
        call<async>(e.ga, [=](G::Vertex& ve){
          ve->post_delta(delta);
        });
      });
      v->activate();
    });
    VLOG(0) << "after gather";
    
    int iteration = 0;
    
    VLOG(0) << "active_count => " << active_count;
    
    while ( active_count > 0 ) GRAPPA_TIME_REGION(iteration_time) {
      if (iteration > FLAGS_max_iterations) { active_count = 0; break; }
      VLOG(1) << "iteration " << std::setw(3) << iteration
              << " -- active:" << active_count;
      
      active_count = 0; // 'apply' deactivates all vertices 
      
      forall(g, [=](G::Vertex& v){
        if (!v->active) return;
        
        // apply
        auto new_val = (1.0 - RESET_PROB) * v->cache + RESET_PROB;
        auto delta = (new_val - v->rank) / v.nadj;
        v->rank = new_val;
        v->active = false;
        
        // scatter
        forall<async>(adj(g,v), [=](G::Edge& e){
          call<async>(e.ga, [=](G::Vertex& ve){
            ve->post_delta(delta);
            if (std::fabs(delta) > TOLERANCE) {
              ve->activate();
            }
          });
        });
      });
      
      iteration++;
    }
#endif
    
    LOG(INFO) << "-- pagerank done";
    
    if (FLAGS_metrics) Metrics::merge_and_print();
    else {
      std::cerr << iteration_time << "\n";
    }
    Metrics::merge_and_dump_to_file();

    if (FLAGS_scale <= 8) {
      g->dump([](std::ostream& o, G::Vertex& v){
        o << "{ rank:" << v->rank << " }";
      });
    }
    
    g->destroy();
  });
  finalize();
}
