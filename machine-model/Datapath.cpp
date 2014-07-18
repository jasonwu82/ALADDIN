#include "./Datapath.h"

Datapath::Datapath(string bench, float cycle_t)
{
  benchName = (char*) bench.c_str();
  cycleTime = cycle_t;
  std::cerr << "-------------------------------" << std::endl;
  std::cerr << "    Initializing Datapath      " << std::endl;
  std::cerr << "-------------------------------" << std::endl;
  string bn(benchName);
  read_gzip_file_no_size(bn + "_microop.gz", microop);
  numTotalNodes = microop.size();
  std::vector<std::string> dynamic_methodid(numTotalNodes, "");
  initDynamicMethodID(dynamic_methodid);

  for (auto dynamic_func_it = dynamic_methodid.begin(), E = dynamic_methodid.end(); dynamic_func_it != E; dynamic_func_it++)
  {
    char func_id[256];
    int count;
    sscanf((*dynamic_func_it).c_str(), "%[^-]-%d\n", func_id, &count);
    if (functionNames.find(func_id) == functionNames.end())
      functionNames.insert(func_id);
  }

  cycle = 0;
}
Datapath::~Datapath()
{
}

void Datapath::setScratchpad(Scratchpad *spad)
{
  std::cerr << "-------------------------------" << std::endl;
  std::cerr << "      Setting ScratchPad       " << std::endl;
  std::cerr << "-------------------------------" << std::endl;
  scratchpad = spad;
}

//optimizationFunctions
void Datapath::setGlobalGraph()
{
  graphName = benchName;
}

void Datapath::clearGlobalGraph()
{
  writeMicroop(microop);
  newLevel.assign(numTotalNodes, 0);
  globalIsolated.assign(numTotalNodes, 1);
  
  regStats.assign(numTotalNodes, {0, 0, 0});
}

void Datapath::globalOptimizationPass()
{
  //graph independent
  ////remove induction variables
  removeInductionDependence();
  
  initBaseAddress();
  removePhiNodes();
  ////complete partition
  completePartition();
  //partition
  scratchpadPartition();
  memoryAmbiguation();
  //loop flattening
  loopFlatten();
  addCallDependence();
  methodGraphBuilder();
  methodGraphSplitter();
}

void Datapath::memoryAmbiguation()
{
  std::cerr << "-------------------------------" << std::endl;
  std::cerr << "      Memory Ambiguation       " << std::endl;
  std::cerr << "-------------------------------" << std::endl;
  
  Graph tmp_graph;
  readGraph(tmp_graph); 
  
  VertexNameMap vertex_to_name = get(boost::vertex_name, tmp_graph);
  EdgeNameMap edge_to_name = get(boost::edge_name, tmp_graph);
  
  std::unordered_map<int, Vertex> name_to_vertex;
  BGL_FORALL_VERTICES(v, tmp_graph, Graph)
    name_to_vertex[get(boost::vertex_name, tmp_graph, v)] = v;
  
  unsigned num_of_edges = boost::num_edges(tmp_graph);
  
  std::unordered_multimap<std::string, std::string> pair_per_load;
  std::unordered_set<std::string> paired_store;
  std::unordered_map<std::string, bool> store_load_pair;

  std::vector<string> instid(numTotalNodes, "");
  std::vector<string> dynamic_methodid(numTotalNodes, "");
  std::vector<string> prev_basic_block(numTotalNodes, "");
  
  initInstID(instid);
  initDynamicMethodID(dynamic_methodid);
  initPrevBasicBlock(prev_basic_block);
  
  std::vector< Vertex > topo_nodes;
  boost::topological_sort(tmp_graph, std::back_inserter(topo_nodes));
  //nodes with no incoming edges to first
  for (auto vi = topo_nodes.rbegin(); vi != topo_nodes.rend(); ++vi)
  {
    int node_id = vertex_to_name[*vi];
    int node_microop = microop.at(node_id);
    if (!is_store_op(node_microop))
      continue;
    //iterate its children to find a load op
    out_edge_iter out_edge_it, out_edge_end;
    for (tie(out_edge_it, out_edge_end) = out_edges(*vi, tmp_graph); out_edge_it != out_edge_end; ++out_edge_it)
    {
      int child_id = vertex_to_name[target(*out_edge_it, tmp_graph)];
      int child_microop = microop.at(child_id);
      if (!is_load_op(child_microop))
        continue;
      string node_dynamic_methodid = dynamic_methodid.at(node_id);
      string load_dynamic_methodid = dynamic_methodid.at(child_id);
      if (node_dynamic_methodid.compare(load_dynamic_methodid) != 0)
        continue;
      
      string store_unique_id (node_dynamic_methodid + "-" + instid.at(node_id) + "-" + prev_basic_block.at(node_id));
      string load_unique_id (load_dynamic_methodid+ "-" + instid.at(child_id) + "-" + prev_basic_block.at(child_id));
      
      if (store_load_pair.find(store_unique_id + "-" + load_unique_id ) != store_load_pair.end())
        continue;
      //add to the pair
      store_load_pair[store_unique_id + "-" + load_unique_id] = 1;
      paired_store.insert(store_unique_id);
      auto load_range = pair_per_load.equal_range(load_unique_id);
      bool found_store = 0;
      for (auto store_it = load_range.first; store_it != load_range.second; store_it++)
      {
        if (store_unique_id.compare(store_it->second) == 0)
        {
          found_store = 1;
          break;
        }
      }
      if (!found_store)
      {
        pair_per_load.insert(make_pair(load_unique_id,store_unique_id));
        //fprintf(stderr, "pair_per_load[%s] = %s\n", load_unique_id.c_str(), store_unique_id.c_str());
      }
    }
  }
  if (store_load_pair.size() == 0)
    return;
  
  std::vector<newEdge> to_add_edges;
  std::unordered_map<std::string, unsigned> last_store;
  
  for (int node_id = 0; node_id < numTotalNodes; node_id++)
  {
    int node_microop = microop.at(node_id);
    if (!is_memory_op(node_microop))
      continue;
    string unique_id (dynamic_methodid.at(node_id) + "-" + instid.at(node_id) + "-" + prev_basic_block.at(node_id));
    if (is_store_op(node_microop))
    {
      auto store_it = paired_store.find(unique_id);
      if (store_it == paired_store.end())
        continue;
      last_store[unique_id] = node_id;
      //fprintf(stderr, "last_store[%s] = %d\n", unique_id.c_str(), node_id);
    }
    else
    {
      assert(is_load_op(node_microop));
      auto load_range = pair_per_load.equal_range(unique_id);
      for (auto load_store_it = load_range.first; load_store_it != load_range.second; ++load_store_it)
      {
        assert(paired_store.find(load_store_it->second) != paired_store.end());
        auto prev_store_it = last_store.find(load_store_it->second);
        if (prev_store_it == last_store.end())
          continue;
        int prev_store_id = prev_store_it->second;
        std::pair<Edge, bool> existed;
        existed = edge(name_to_vertex[prev_store_id], name_to_vertex[node_id], tmp_graph);
        if (existed.second == false)
        {
          //fprintf(stderr, "ambiguation edge :%d,%d (%s,%s)\n", prev_store_id, node_id, load_store_it->second.c_str(), load_store_it->first.c_str());
          to_add_edges.push_back({prev_store_id, node_id, -1, 1});
          dynamicMemoryOps.insert(load_store_it->second + "-" + prev_basic_block.at(prev_store_id));
          dynamicMemoryOps.insert(load_store_it->first + "-" + prev_basic_block.at(node_id));
        }
      }
    }
  }
  writeGraphWithNewEdges(to_add_edges, num_of_edges);
}

void Datapath::removePhiNodes()
{
  std::cerr << "-------------------------------" << std::endl;
  std::cerr << "       Remove PHI Nodes        " << std::endl;
  std::cerr << "-------------------------------" << std::endl;
  
  Graph tmp_graph;
  readGraph(tmp_graph); 
  
  VertexNameMap vertex_to_name = get(boost::vertex_name, tmp_graph);
  EdgeNameMap edge_to_name = get(boost::edge_name, tmp_graph);
  
  unsigned num_of_edges = boost::num_edges(tmp_graph);
  
  std::vector<bool> to_remove_edges(num_of_edges, 0);
  std::vector<newEdge> to_add_edges;
  
  std::vector<int> edge_parid(num_of_edges, 0);
  std::vector<unsigned> edge_latency(num_of_edges, 0);
  
  initEdgeLatency(edge_latency);
  initEdgeParID(edge_parid);
  
  vertex_iter vi, vi_end;
  int removed_phi = 0;
  for (tie(vi, vi_end) = vertices(tmp_graph); vi != vi_end; ++vi)
  {
    int node_id = vertex_to_name[*vi];
    int node_microop = microop.at(node_id);
    if (node_microop != LLVM_IR_PHI)
      continue;
    //find its children
    std::vector< pair<unsigned, unsigned> > phi_child;

    out_edge_iter out_edge_it, out_edge_end;
    for (tie(out_edge_it, out_edge_end) = out_edges(*vi, tmp_graph); out_edge_it != out_edge_end; ++out_edge_it)
    {
      int edge_id = edge_to_name[*out_edge_it];
      int child_id = vertex_to_name[target(*out_edge_it, tmp_graph)];
      int child_microop = microop.at(child_id);
      to_remove_edges.at(edge_id) = 1;
      //fprintf(stderr, "curr:%d,%d,child:%d,%d,%d\n", node_id, node_microop, child_id, child_microop, edge_id);
      phi_child.push_back(make_pair(child_id, edge_id));
    }
    if (phi_child.size() == 0)
      continue;

    //find its parents
    in_edge_iter in_edge_it, in_edge_end;
    for (tie(in_edge_it, in_edge_end) = in_edges(*vi, tmp_graph); in_edge_it != in_edge_end; ++in_edge_it)
    {
      int parent_id = vertex_to_name[source(*in_edge_it, tmp_graph)];
      int parent_microop = microop.at(parent_id);
      int edge_id = edge_to_name[*in_edge_it];
      //fprintf(stderr, "curr:%d,%d,parent:%d,%d,%d\n", node_id, node_microop, parent_id, parent_microop, edge_id);
      
      to_remove_edges.at(edge_id) = 1;
      for (auto child_it = phi_child.begin(), chil_E = phi_child.end(); child_it != chil_E; ++child_it)
      {
        int child_id = child_it->first;
        int child_edge_id  = child_it->second;
        to_add_edges.push_back({parent_id, child_id, edge_parid.at(child_edge_id), edge_latency.at(child_edge_id)});
        //fprintf(stderr, "adding edges between %d and %d\n", parent_id, child_id);   
      }
    }
    removed_phi++;
  }
  int curr_num_of_edges = writeGraphWithIsolatedEdges(to_remove_edges);
  writeGraphWithNewEdges(to_add_edges, curr_num_of_edges);
  cleanLeafNodes();

}
void Datapath::initBaseAddress()
{
  std::cerr << "-------------------------------" << std::endl;
  std::cerr << "       Init Base Address       " << std::endl;
  std::cerr << "-------------------------------" << std::endl;
  //set graph
  Graph tmp_graph;
  readGraph(tmp_graph); 
  unsigned num_of_edges = boost::num_edges(tmp_graph);
  std::vector<unsigned> edge_latency(num_of_edges, 0);
  initEdgeLatency(edge_latency);

  VertexNameMap vertex_to_name = get(boost::vertex_name, tmp_graph);
  EdgeNameMap edge_to_name = get(boost::edge_name, tmp_graph);

  std::unordered_map<unsigned, pair<string, unsigned> > getElementPtr;
  initGetElementPtr(getElementPtr);
  
  vertex_iter vi, vi_end;
  for (tie(vi, vi_end) = vertices(tmp_graph); vi != vi_end; ++vi)
  {
    int node_id = vertex_to_name[*vi];
    int node_microop = microop.at(node_id);
    if (!is_memory_op(node_microop))
      continue;
    //iterate its parents
    in_edge_iter in_edge_it, in_edge_end;
    bool flag_GEP = 0;
    for (tie(in_edge_it, in_edge_end) = in_edges(*vi , tmp_graph); in_edge_it != in_edge_end; ++in_edge_it)
    {
      int parent_id = vertex_to_name[source(*in_edge_it, tmp_graph)];
      int parent_microop = microop.at(parent_id);
      if (parent_microop == LLVM_IR_GetElementPtr)
      {
        baseAddress[node_id] = getElementPtr[parent_id];
        //remove address calculation directly
        int edge_id = edge_to_name[*in_edge_it];
        edge_latency.at(edge_id) = 0;
        flag_GEP = 1;
        break;
      }
    }
    if (!flag_GEP)
      baseAddress[node_id] = getElementPtr[node_id];
  }
  writeEdgeLatency(edge_latency);
}

void Datapath::loopFlatten()
{
  std::unordered_set<int> flatten_config;
  if (!readFlattenConfig(flatten_config))
    return;
  std::cerr << "-------------------------------" << std::endl;
  std::cerr << "         Loop Flatten         " << std::endl;
  std::cerr << "-------------------------------" << std::endl;
  std::vector<int> lineNum(numTotalNodes, -1);
  initLineNum(lineNum);
  
  int num_of_conversion= 0;

  for(int node_id = 0; node_id < numTotalNodes; node_id++)
  {
    int node_linenum = lineNum.at(node_id);
    auto it = flatten_config.find(node_linenum);
    if (it == flatten_config.end())
      continue;
    if (is_compute_op(microop.at(node_id)))
      microop.at(node_id) = LLVM_IR_Move;
      //num_of_convresion++;
  }
}
/*
 * Modify: graph, edgetype, edgelatency, baseAddress, microop
 * */
void Datapath::completePartition()
{
  std::unordered_set<string> comp_part_config;
  if (!readCompletePartitionConfig(comp_part_config))
    return;
  
  std::cerr << "-------------------------------" << std::endl;
  std::cerr << "        Mem to Reg Conv        " << std::endl;
  std::cerr << "-------------------------------" << std::endl;
  
  int changed_nodes = 0;
  //set graph
  Graph tmp_graph;
  readGraph(tmp_graph); 

  VertexNameMap vertex_to_name = get(boost::vertex_name, tmp_graph);
  EdgeNameMap edge_to_name = get(boost::edge_name, tmp_graph);
  
  unsigned num_of_edges = boost::num_edges(tmp_graph);
  
 //read edgetype.second
  std::vector<bool> to_remove_edges(num_of_edges, 0);
  
  vertex_iter vi, vi_end;
  for (tie(vi, vi_end) = vertices(tmp_graph); vi != vi_end; ++vi)
  {
    int node_id = vertex_to_name[*vi];
    int node_microop = microop.at(node_id);
    if (!is_memory_op(node_microop))
      continue;
    //fprintf (stderr, "nodeid:%d\n", node_id);
    assert(baseAddress.find(node_id) != baseAddress.end());
    string base_label = baseAddress[node_id].first;
    //fprintf(stderr, "node:%d,label:%s\n", node_id, base_label.c_str());
    if (comp_part_config.find(base_label) == comp_part_config.end())
      continue;
    changed_nodes++;
    //iterate its parents
    in_edge_iter in_edge_it, in_edge_end;
    for (tie(in_edge_it, in_edge_end) = in_edges(*vi , tmp_graph); in_edge_it != in_edge_end; ++in_edge_it)
    {
      int parent_id = vertex_to_name[source(*in_edge_it, tmp_graph)];
      int parent_microop = microop.at(parent_id);
      if (parent_microop == LLVM_IR_GetElementPtr)
      {
        int edge_id = edge_to_name[*in_edge_it];
        to_remove_edges.at(edge_id) = 1;
      }
    }
    //fprintf(stderr, "completely partition: %d, %s", node_id, base_label.c_str());
    baseAddress.erase(node_id);
    microop.at(node_id) = LLVM_IR_Move;
  }
  writeGraphWithIsolatedEdges(to_remove_edges);
  cleanLeafNodes();
}
void Datapath::cleanLeafNodes()
{
  //set graph
  Graph tmp_graph;
  readGraph(tmp_graph); 
  
  VertexNameMap vertex_to_name = get(boost::vertex_name, tmp_graph);
  unsigned num_of_nodes = boost::num_vertices(tmp_graph);
  /*track the number of children each node has*/
  vector<int> num_of_children(num_of_nodes, 0);
  std::vector<bool> tmp_isolated(num_of_nodes, 0);
  vertex_iter vi, vi_end;
  for (tie(vi, vi_end) = vertices(tmp_graph); vi != vi_end; ++vi)
  {
    int node_id = vertex_to_name[*vi];
    num_of_children.at(node_id) = boost::out_degree(*vi, tmp_graph);
    if (boost::degree(*vi, tmp_graph) == 0)
      tmp_isolated.at(node_id) = 1;
  }
  
  
  unordered_set<unsigned> to_remove_nodes;
  
  std::vector< Vertex > topo_nodes;
  boost::topological_sort(tmp_graph, std::back_inserter(topo_nodes));
  //bottom nodes first
  for (auto vi = topo_nodes.begin(); vi != topo_nodes.end(); ++vi)
  {
    int node_id = vertex_to_name[*vi];
    if (tmp_isolated.at(node_id))
      continue;
    assert(num_of_children.at(node_id) >= 0);
    if (num_of_children.at(node_id) == 0 
      && microop.at(node_id) != LLVM_IR_SilentStore
      //&& microop.at(node_id) != LLVM_IR_DynamicStore
      && microop.at(node_id) != LLVM_IR_Store
      && microop.at(node_id) != LLVM_IR_Ret 
      && microop.at(node_id) != LLVM_IR_Br
      && microop.at(node_id) != LLVM_IR_Switch
      && microop.at(node_id) != LLVM_IR_Call)
    {
      to_remove_nodes.insert(node_id); 
      //iterate its parents
      in_edge_iter in_edge_it, in_edge_end;
      for (tie(in_edge_it, in_edge_end) = in_edges(*vi, tmp_graph); in_edge_it != in_edge_end; ++in_edge_it)
      {
        int parent_id = vertex_to_name[source(*in_edge_it, tmp_graph)];
        assert(num_of_children.at(parent_id) != 0);
        num_of_children.at(parent_id)--;
      }
    }
  }
  writeGraphWithIsolatedNodes(to_remove_nodes);
}

void Datapath::removeInductionDependence()
{
  //set graph
  std::cerr << "-------------------------------" << std::endl;
  std::cerr << "  Remove Induction Dependence  " << std::endl;
  std::cerr << "-------------------------------" << std::endl;
  
  Graph tmp_graph;
  readGraph(tmp_graph); 
  
  VertexNameMap vertex_to_name = get(boost::vertex_name, tmp_graph);
  EdgeNameMap edge_to_name = get(boost::edge_name, tmp_graph);
  
  unsigned num_of_edges = boost::num_edges(tmp_graph);
  std::vector<string> instid(numTotalNodes, "");
  initInstID(instid);

  std::vector<unsigned> edge_latency(num_of_edges, 0);
  initEdgeLatency(edge_latency);
  
  int removed_edges = 0;
  
  std::vector< Vertex > topo_nodes;
  boost::topological_sort(tmp_graph, std::back_inserter(topo_nodes));
  //nodes with no incoming edges to first
  for (auto vi = topo_nodes.rbegin(); vi != topo_nodes.rend(); ++vi)
  {
    int node_id = vertex_to_name[*vi];
    string node_instid = instid.at(node_id);
    
    if (node_instid.find("indvars") != std::string::npos)
      continue;
    //check its children, update the edge latency between them to 0
    out_edge_iter out_edge_it, out_edge_end;
    for (tie(out_edge_it, out_edge_end) = out_edges(*vi, tmp_graph); out_edge_it != out_edge_end; ++out_edge_it)
    {
      int edge_id = edge_to_name[*out_edge_it];
      edge_latency.at(edge_id) = 0;
      removed_edges++;
    }
  }
  writeEdgeLatency(edge_latency);

}

void Datapath::methodGraphBuilder()
{
  std::cerr << "-------------------------------" << std::endl;
  std::cerr << "     Split into Functions      " << std::endl;
  std::cerr << "-------------------------------" << std::endl;
  string call_file_name(benchName);
  call_file_name += "_method_call_graph";
  ifstream call_file;
  call_file.open(call_file_name);

  std::vector<callDep> method_dep;
  std::vector<std::string> node_att;
  string prev_callee;
  while (!call_file.eof())
  {
    string wholeline;
    getline(call_file, wholeline);
    if (wholeline.size() == 0)
      break;
    char curr[256];
    char next[256];
    int dynamic_id;
    sscanf(wholeline.c_str(), "%d,%[^,],%[^,]\n", &dynamic_id, curr, next);
    
    string caller_method(curr);
    string callee_method(next);
    
    node_att.push_back(caller_method);
    if (method_dep.size())
      method_dep.push_back({prev_callee, callee_method, dynamic_id});
    method_dep.push_back({caller_method, callee_method, dynamic_id});
    prev_callee = callee_method;
  }
  call_file.close();
  //write output
  ofstream edgelist_file;
  //, edge_att_file, node_att_file;

  string graph_file(benchName);
  graph_file += "_method_graph";
  edgelist_file.open(graph_file);
  edgelist_file << "digraph MethodGraph {" << std::endl;
  
  for(unsigned i = 0; i < node_att.size() ; i++)
    edgelist_file << "\"" << node_att.at(i) << "\"" << ";" << std::endl;
  
  for (auto it = method_dep.begin(); it != method_dep.end(); ++it)
  {
    edgelist_file << "\"" << it->caller  << "\"" << " -> " 
                  << "\"" << it->callee  << "\"" 
                  << " [e_inst = " << it->callInstID << "];" << std::endl;
  }
  edgelist_file << "}" << endl;
  edgelist_file.close();
}
void Datapath::methodGraphSplitter()
{
  std::cerr << "-------------------------------" << std::endl;
  std::cerr << "    Generate Functions DDDG    " << std::endl;
  std::cerr << "-------------------------------" << std::endl;
  //set graph
  MethodGraph tmp_method_graph;
  readMethodGraph(tmp_method_graph);
  
  MethodVertexNameMap vertex_to_name = get(boost::vertex_name, tmp_method_graph);
  MethodEdgeNameMap edge_to_name = get(boost::edge_name, tmp_method_graph);

  unsigned num_method_nodes = num_vertices(tmp_method_graph);
  
  std::vector<string> dynamic_methodid(numTotalNodes, "");
  initDynamicMethodID(dynamic_methodid);

  std::vector<bool> updated(numTotalNodes, 0);
  
  std::vector< MethodVertex > topo_nodes;
  boost::topological_sort(tmp_method_graph, std::back_inserter(topo_nodes));
  ofstream method_order;
  string bn(benchName);
  method_order.open(bn + "_method_order");
  
  //set graph
  unordered_map<string, edgeAtt > full_graph ;
  initializeGraphInMap(full_graph);
  //bottom node first
  for (auto vi = topo_nodes.begin(); vi != topo_nodes.end(); ++vi)
  {
    string method_name = vertex_to_name[*vi]; 
    //only one caller for each dynamic function
    //assert (boost::in_degree(*vi, tmp_method_graph) <= 1);
    if (boost::in_degree(*vi, tmp_method_graph) == 0)
      break;
    
    method_in_edge_iter in_edge_it, in_edge_end;
    tie(in_edge_it, in_edge_end) = in_edges(*vi, tmp_method_graph);
    int call_inst = edge_to_name[*in_edge_it];
    method_order << method_name << "," <<
      vertex_to_name[source(*in_edge_it, tmp_method_graph)] << "," << call_inst << std::endl;
    std::unordered_set<int> to_split_nodes;
    
    unsigned min_node = call_inst + 1; 
    unsigned max_node = call_inst + 1;
    for (unsigned node_id = call_inst + 1; node_id < numTotalNodes; node_id++)
    {
      string node_dynamic_methodid = dynamic_methodid.at(node_id);
      if (node_dynamic_methodid.compare(method_name) != 0  )
        continue;
      if (microop.at(node_id) == LLVM_IR_Ret)
        break;
      to_split_nodes.insert(node_id);
      if (node_id > max_node)
        max_node = node_id;
    }
    
    unordered_map<string, edgeAtt> current_graph;
    auto graph_it = full_graph.begin(); 
    while (graph_it != full_graph.end())
    {
      int from, to;
      sscanf(graph_it->first.c_str(), "%d-%d", &from, &to);
      assert (from != to);
      if ( ( from < min_node && to < min_node )
        || (from > max_node && to > max_node))
      {
        graph_it++;
        continue;
      }
      //cerr << "checking edge: " << from << "," << to << "," << call_inst << endl;
      auto split_from_it = to_split_nodes.find(from);
      auto split_to_it = to_split_nodes.find(to);
      if (split_from_it == to_split_nodes.end() 
          && split_to_it == to_split_nodes.end())
      {
        //cerr << "non of them in the new graph, do nothing" << endl;
        graph_it++;
        continue;
      }
      else if (split_from_it == to_split_nodes.end() 
          && split_to_it != to_split_nodes.end())
      {
        //update the full graph: remove edge between from to to, add edge
        //between from to call_inst
        ostringstream oss;
        oss << from << "-" << call_inst;
        if(from != call_inst && full_graph.find(oss.str()) == full_graph.end())
          full_graph[oss.str()] = { graph_it->second.parid, graph_it->second.latency};
        graph_it = full_graph.erase(graph_it);
      }
      else if (split_from_it != to_split_nodes.end() 
          && split_to_it == to_split_nodes.end())
      {
        //update the full graph: remove edge between from to to, add edge
        ostringstream oss;
        oss << call_inst << "-" << to;
        if (call_inst != to && full_graph.find(oss.str()) != full_graph.end())
          full_graph[oss.str()] = {graph_it->second.parid, graph_it->second.latency};
        graph_it = full_graph.erase(graph_it);
      }
      else
      {
        //write to the new graph
        if (current_graph.find(graph_it->first) == current_graph.end())
          current_graph[graph_it->first] = {graph_it->second.parid, graph_it->second.latency};
        graph_it = full_graph.erase(graph_it);
      }
    }
    writeGraphInMap(current_graph, bn + "_" + method_name);
    std::unordered_map<string, edgeAtt>().swap(current_graph);
    std::unordered_set<int>().swap(to_split_nodes);
  }
  
  method_order.close();
  writeGraphInMap(full_graph, bn);
}
void Datapath::addCallDependence()
{
  //set graph
  Graph tmp_graph;
  readGraph(tmp_graph);
  
  std::vector<newEdge> to_add_edges;
  int last_call = -1;
  for(int node_id = 0; node_id < numTotalNodes; node_id++)
  {
    if (microop.at(node_id) == LLVM_IR_Call)
    {
      if (last_call != -1)
        to_add_edges.push_back({last_call, node_id, -1, 1});
      last_call = node_id;
    }
  }
  int num_of_edges = num_edges(tmp_graph);
  writeGraphWithNewEdges(to_add_edges, num_of_edges);

}

/*
 * Modify: benchName_membase.gz
 * */
void Datapath::scratchpadPartition()
{
  //read the partition config file to get the address range
  // <base addr, <type, part_factor> > 
  std::unordered_map<string, partitionEntry> part_config;
  if (!readPartitionConfig(part_config))
    return;

  std::cerr << "-------------------------------" << std::endl;
  std::cerr << "      ScratchPad Partition     " << std::endl;
  std::cerr << "-------------------------------" << std::endl;
  string bn(benchName);
  
  string partition_file;
  partition_file = bn + "_partition_config";

  std::unordered_map<unsigned, pair<unsigned, unsigned> > address;
  initAddressAndSize(address);
  //set scratchpad
  for(auto it = part_config.begin(); it!= part_config.end(); ++it)
  {
    string base_addr = it->first;
    unsigned size = it->second.array_size; //num of words
    unsigned p_factor = it->second.part_factor;
    unsigned per_size = ceil(size / p_factor);
    
    for ( unsigned i = 0; i < p_factor ; i++)
    {
      ostringstream oss;
      oss << base_addr << "-" << i;
      scratchpad->setScratchpad(oss.str(), per_size);
    }
  }
  for(unsigned node_id = 0; node_id < numTotalNodes; node_id++)
  {
    int node_microop = microop.at(node_id);
    if (!is_memory_op(node_microop))
      continue;
    
    assert(baseAddress.find(node_id) != baseAddress.end());
    string base_label  = baseAddress[node_id].first;
    unsigned base_addr = baseAddress[node_id].second;
    
    auto part_it = part_config.find(base_label);
    if (part_it != part_config.end())
    {
      string p_type = part_it->second.type;
      assert((!p_type.compare("block")) || (!p_type.compare("cyclic")));
      
      unsigned num_of_elements = part_it->second.array_size;
      unsigned p_factor        = part_it->second.part_factor;
      unsigned abs_addr        = address[node_id].first;
      unsigned data_size       = address[node_id].second;
      unsigned rel_addr        = (abs_addr - base_addr ) / data_size; 
      if (!p_type.compare("block"))  //block partition
      {
        ostringstream oss;
        unsigned num_of_elements_in_2 = next_power_of_two(num_of_elements);
        oss << base_label << "-" << (int) (rel_addr / ceil (num_of_elements_in_2  / p_factor)) ;
        baseAddress[node_id].first = oss.str();
      }
      else // (!p_type.compare("cyclic")), cyclic partition
      {
        ostringstream oss;
        oss << base_label << "-" << (rel_addr) % p_factor;
        baseAddress[node_id].first = oss.str();
      }
    }
  }
}
//called in the end of the whole flow
void Datapath::dumpStats()
{
  writeMicroop(microop);
  writeFinalLevel();
  writeGlobalIsolated();
  writePerCycleActivity();
}

//localOptimizationFunctions
void Datapath::setGraphName(string graph_name, int min)
{
  std::cerr << "=============================================" << std::endl;
  std::cerr << "      Optimizing...            " << graph_name << std::endl;
  std::cerr << "=============================================" << std::endl;
  graphName = (char*)graph_name.c_str();
  minNode = min;
}

void Datapath::optimizationPass()
{
  loopUnrolling();
  removeSharedLoads();
  storeBuffer();
  removeRepeatedStores();
  treeHeightReduction();
  
}

void Datapath::loopUnrolling()
{
  std::unordered_map<int, int > unrolling_config;
  if (!readUnrollingConfig(unrolling_config))
  {
    std::cerr << "Loop Unrolling is undefined. Default unroll all the loops completely" << endl;
    return ;
  }

  std::cerr << "-------------------------------" << std::endl;
  std::cerr << "         Loop Unrolling        " << std::endl;
  std::cerr << "-------------------------------" << std::endl;
  //set graph
  Graph tmp_graph;
  readGraph(tmp_graph); 
  
  VertexNameMap vertex_to_name = get(boost::vertex_name, tmp_graph);
  EdgeNameMap edge_to_name = get(boost::edge_name, tmp_graph);

  //unordered_map
  //custom allocator, boost::slab allocator 
  std::unordered_map<int, Vertex> name_to_vertex;
  BGL_FORALL_VERTICES(v, tmp_graph, Graph)
    name_to_vertex[get(boost::vertex_name, tmp_graph, v)] = v;
  
  unsigned num_of_edges = boost::num_edges(tmp_graph);
  unsigned num_of_nodes = boost::num_vertices(tmp_graph);
  
  std::vector<string> instid(num_of_nodes, "");
  initInstID(instid);
  
  std::vector<int> lineNum(num_of_nodes, -1);
  initLineNum(lineNum);

  std::vector<unsigned> edge_latency(num_of_edges, 0);
  initEdgeLatency(edge_latency);
  
  std::vector<bool> tmp_isolated(num_of_nodes, 0);
  unsigned max_node_id = minNode;
  
  vertex_iter vi, vi_end;
  for (tie(vi, vi_end) = vertices(tmp_graph); vi != vi_end; ++vi)
  {
    unsigned node_id = vertex_to_name[*vi];
    if (boost::degree(*vi, tmp_graph) == 0)
      tmp_isolated.at(node_id) = 1;
    else
      if (node_id > max_node_id)
        max_node_id = node_id;
  }

  int add_unrolling_edges = 0;
  
  ofstream loop_bound;
  string file_name(graphName);
  file_name += "_loop_bound";
  loop_bound.open(file_name.c_str());
  bool first = 0;
  //cerr << "minNode, num_of_nodes" << minNode << "," << num_of_nodes << endl;
  std::unordered_map<string, unsigned> inst_dynamic_counts;

  for(unsigned node_id = minNode; node_id < max_node_id; node_id++)
  {
    if(tmp_isolated.at(node_id))
      continue;
    if (!first)
    {
      first = 1;
      loop_bound << node_id << endl;
    }
    int node_linenum = lineNum.at(node_id);
    auto unroll_it = unrolling_config.find(node_linenum);
    if (unroll_it == unrolling_config.end())
      continue;

    int node_microop = microop.at(node_id);
    string node_instid = instid.at(node_id);
    
    if (node_microop == LLVM_IR_Add && node_instid.find("indvars") != std::string::npos)
      microop.at(node_id) = LLVM_IR_IndexAdd;
    
    int factor = unroll_it->second;
    char unique_inst_id[256];

    sprintf(unique_inst_id, "%s-%d", node_instid.c_str(), node_linenum);
    //fprintf(stderr, "unique id: %s\n", unique_inst_id);
    auto it = inst_dynamic_counts.find(unique_inst_id);
    if (it == inst_dynamic_counts.end())
    {
      inst_dynamic_counts[unique_inst_id] = 0;
      it = inst_dynamic_counts.find(unique_inst_id);
    }
    else
      it->second++;
    //fprintf(stderr, "node_id:%d,counts:%d,factor:%d\n", node_id, it->second, factor);
    //time to roll
    if (it->second % factor == 0)
    {
      //fprintf(stderr, "node_id:%d,counts:%d,factor:%d, op:%d\n", node_id, it->second, factor, node_microop);
      if (node_microop == LLVM_IR_Add && node_instid.find("indvars") != std::string::npos)
      {
        //fprintf(stderr, "loop bound: node_id:%d\n", node_id);
        loop_bound << node_id << endl;
        microop.at(node_id) = LLVM_IR_IndexAdd;
      }

      if (factor == 1)
        continue;
      
      Vertex node = name_to_vertex[node_id];
      out_edge_iter out_edge_it, out_edge_end;
      for (tie(out_edge_it, out_edge_end) = out_edges(node, tmp_graph); out_edge_it != out_edge_end; ++out_edge_it)
      {
        int child_id = vertex_to_name[target(*out_edge_it, tmp_graph)];
        string child_instid = instid.at(child_id);
        int child_linenum = lineNum.at(child_id);
        int child_microop = microop.at(child_id);
        if ((child_instid.compare(node_instid) == 0) 
           && (child_linenum == node_linenum))
        {
          int edge_id = edge_to_name[*out_edge_it];
          edge_latency[edge_id] = 1;
          microop.at(child_id) = LLVM_IR_IndexAdd;
          add_unrolling_edges++;
        }
      }
    }
  }
  loop_bound << num_of_nodes << endl;
  loop_bound.close();
}

void Datapath::removeSharedLoads()
{
  std::vector<int> loop_bound;
  string file_name(graphName);
  file_name += "_loop_bound";
  read_file(file_name, loop_bound);
  std::cerr << "-------------------------------" << std::endl;
  std::cerr << "          Load Buffer          " << std::endl;
  std::cerr << "-------------------------------" << std::endl;
  //set graph
  Graph tmp_graph;
  readGraph(tmp_graph); 
  VertexNameMap vertex_to_name = get(boost::vertex_name, tmp_graph);
  EdgeNameMap edge_to_name = get(boost::edge_name, tmp_graph);
  
  unsigned num_of_edges = boost::num_edges(tmp_graph);
  unsigned num_of_nodes = boost::num_vertices(tmp_graph);
  
  std::map<int, Vertex> name_to_vertex;
  BGL_FORALL_VERTICES(v, tmp_graph, Graph)
    name_to_vertex[get(boost::vertex_name, tmp_graph, v)] = v;
  
  std::unordered_map<unsigned, unsigned> address;
  std::vector<unsigned> edge_latency(num_of_edges, 0);
  std::vector<int> edge_parid(num_of_edges, 0);

  initAddress(address);
  initEdgeLatency(edge_latency);
  initEdgeParID(edge_parid);

  std::vector<bool> tmp_isolated(num_of_nodes, 0);
  vertex_iter vi, vi_end;
  unsigned max_node_id = minNode;
  for (tie(vi, vi_end) = vertices(tmp_graph); vi != vi_end; ++vi)
  {
    unsigned node_id = vertex_to_name[*vi];
    if (boost::degree(*vi, tmp_graph) == 0)
      tmp_isolated.at(node_id) = 1;
    else
      if (node_id > max_node_id)
        max_node_id = node_id;
  }
  
  std::vector<bool> to_remove_edges(num_of_edges, 0);
  std::vector<newEdge> to_add_edges;

  int shared_loads = 0;
  auto loop_bound_it = loop_bound.begin();
  
  int node_id = minNode;
  while ( (unsigned)node_id < max_node_id)
  {
    std::unordered_map<unsigned, int> address_loaded;
    //fprintf(stderr, "node_id:%d,loop boud:%d,maxNode:%d\n", node_id, *loop_bound_it, max_node_id);
    while (node_id < *loop_bound_it &&  (unsigned) node_id < max_node_id)
    {
      if(tmp_isolated.at(node_id))
      {
        node_id++;
        continue;
      }
      int node_microop = microop.at(node_id);
      unsigned node_address = address[node_id];
      auto addr_it = address_loaded.find(node_address);
      if (is_store_op(node_microop) && addr_it != address_loaded.end())
        address_loaded.erase(addr_it);
      else if (is_load_op(node_microop))
      {
        if (addr_it == address_loaded.end())
          address_loaded[node_address] = node_id;
        else
        {
          shared_loads++;
          microop.at(node_id) = LLVM_IR_Move;
          int prev_load = addr_it->second;
          //iterate throught its children
          Vertex load_node = name_to_vertex[node_id];
          
          out_edge_iter out_edge_it, out_edge_end;
          for (tie(out_edge_it, out_edge_end) = out_edges(load_node, tmp_graph); out_edge_it != out_edge_end; ++out_edge_it)
          {
            int child_id = vertex_to_name[target(*out_edge_it, tmp_graph)];
            int edge_id = edge_to_name[*out_edge_it];
            std::pair<Edge, bool> existed;
            existed = edge(name_to_vertex[prev_load], name_to_vertex[node_id], tmp_graph);
            if (existed.second == false)
              to_add_edges.push_back({prev_load, child_id, edge_parid.at(edge_id), edge_latency.at(edge_id)});
            to_remove_edges[edge_id] = 1;
          }
          
          in_edge_iter in_edge_it, in_edge_end;
          for (tie(in_edge_it, in_edge_end) = in_edges(load_node, tmp_graph); in_edge_it != in_edge_end; ++in_edge_it)
          {
            int edge_id = edge_to_name[*in_edge_it];
            to_remove_edges.at(edge_id) = 1;
          }
        }
      }
      node_id++;
    }
    loop_bound_it++;
    if (loop_bound_it == loop_bound.end() )
      break;
  }
  edge_latency.clear();
  edge_parid.clear();
  int curr_num_of_edges = writeGraphWithIsolatedEdges(to_remove_edges);
  writeGraphWithNewEdges(to_add_edges, curr_num_of_edges);
  cleanLeafNodes();
}

void Datapath::storeBuffer()
{
  std::cerr << "-------------------------------" << std::endl;
  std::cerr << "          Store Buffer         " << std::endl;
  std::cerr << "-------------------------------" << std::endl;
  //set graph
  Graph tmp_graph;
  readGraph(tmp_graph); 
  
  EdgeNameMap edge_to_name = get(boost::edge_name, tmp_graph);
  VertexNameMap vertex_to_name = get(boost::vertex_name, tmp_graph);
  
  std::map<int, Vertex> name_to_vertex;
  BGL_FORALL_VERTICES(v, tmp_graph, Graph)
    name_to_vertex[get(boost::vertex_name, tmp_graph, v)] = v;
  
  unsigned num_of_edges = boost::num_edges(tmp_graph);
  unsigned num_of_nodes = boost::num_vertices(tmp_graph);
  
  std::vector<int> edge_parid(num_of_edges, 0);
  std::vector<unsigned> edge_latency(num_of_edges, 0);
  
  initEdgeLatency(edge_latency);
  initEdgeParID(edge_parid);
  
  std::vector<string> instid(numTotalNodes, "");
  std::vector<string> dynamic_methodid(numTotalNodes, "");
  std::vector<string> prev_basic_block(numTotalNodes, "");
  
  initInstID(instid);
  initDynamicMethodID(dynamic_methodid);
  initPrevBasicBlock(prev_basic_block);
  
  std::vector<int> loop_bound;
  string file_name(graphName);
  
  file_name += "_loop_bound";
  read_file(file_name, loop_bound);
  
  std::vector<bool> tmp_isolated(num_of_nodes, 0);
  vertex_iter vi, vi_end;
  unsigned max_node_id = minNode;
  for (tie(vi, vi_end) = vertices(tmp_graph); vi != vi_end; ++vi)
  {
    unsigned node_id = vertex_to_name[*vi];
    if (boost::degree(*vi, tmp_graph) == 0)
      tmp_isolated.at(node_id) = 1;
    else
      if (node_id > max_node_id)
        max_node_id = node_id;
  }
  
  std::vector<bool> to_remove_edges(num_of_edges, 0);
  std::vector<newEdge> to_add_edges;

  int buffered_stores = 0;
  auto loop_bound_it = loop_bound.begin();
  
  int node_id = minNode;
  while (node_id < max_node_id)
  {
    while (node_id < *loop_bound_it && node_id < max_node_id)
    {
      if(tmp_isolated.at(node_id))
      {
        node_id++;
        continue;
      }
      int node_microop = microop.at(node_id);
      if (is_store_op(node_microop))
      {
        Vertex node = name_to_vertex[node_id];
        out_edge_iter out_edge_it, out_edge_end;
        std::vector<unsigned> store_child;
        for (tie(out_edge_it, out_edge_end) = out_edges(node, tmp_graph); out_edge_it != out_edge_end; ++out_edge_it)
        {
          int child_id = vertex_to_name[target(*out_edge_it, tmp_graph)];
          int child_microop = microop.at(child_id);
          if (is_load_op(child_microop))
          {
            string load_unique_id (dynamic_methodid.at(child_id) + "-" + instid.at(child_id) + "-" + prev_basic_block.at(child_id));
            if (dynamicMemoryOps.find(load_unique_id) != dynamicMemoryOps.end())
              continue;
            if (child_id >= (unsigned)*loop_bound_it )
              continue;
            else
              store_child.push_back(child_id);
          }
        }
        
        if (store_child.size() > 0)
        {
          buffered_stores++;
          
          in_edge_iter in_edge_it, in_edge_end;
          int store_parent = num_of_nodes;
          for (tie(in_edge_it, in_edge_end) = in_edges(node, tmp_graph); in_edge_it != in_edge_end; ++in_edge_it)
          {
            int edge_id = edge_to_name[*in_edge_it];
            int parent_id = vertex_to_name[source(*in_edge_it, tmp_graph)];
            int parid = edge_parid.at(edge_id);
            //parent node that generates value
            if (parid == 1)
            {
              store_parent = parent_id;
              break;
            }
          }
          
          if (store_parent != num_of_nodes)
          {
            for (unsigned i = 0; i < store_child.size(); ++i)
            {
              unsigned load_id = store_child.at(i);
              
              Vertex load_node = name_to_vertex[load_id];
              
              out_edge_iter out_edge_it, out_edge_end;
              for (tie(out_edge_it, out_edge_end) = out_edges(load_node, tmp_graph); out_edge_it != out_edge_end; ++out_edge_it)
              {
                int edge_id = edge_to_name[*out_edge_it];
                int child_id = vertex_to_name[target(*out_edge_it, tmp_graph)];
                to_remove_edges.at(edge_id) = 1;
                to_add_edges.push_back({store_parent, child_id, edge_parid.at(edge_id), edge_latency.at(edge_id)});
              }
              
              for (tie(in_edge_it, in_edge_end) = in_edges(load_node, tmp_graph); in_edge_it != in_edge_end; ++in_edge_it)
              {
                int edge_id = edge_to_name[*in_edge_it];
                to_remove_edges.at(edge_id) = 1;
              }
            }
          }
        }
      }
      ++node_id; 
    }
    loop_bound_it++;
    if (loop_bound_it == loop_bound.end() )
      break;
  }
  edge_latency.clear();
  edge_parid.clear();
  int curr_num_of_edges = writeGraphWithIsolatedEdges(to_remove_edges);
  writeGraphWithNewEdges(to_add_edges, curr_num_of_edges);
  cleanLeafNodes();
}

void Datapath::removeRepeatedStores()
{
  std::vector<int> loop_bound;
  string file_name(graphName);
  file_name += "_loop_bound";
  read_file(file_name, loop_bound);
  
  if (loop_bound.size() < 1)
    return;
  std::cerr << "-------------------------------" << std::endl;
  std::cerr << "     Remove Repeated Store     " << std::endl;
  std::cerr << "-------------------------------" << std::endl;
  //set graph
  Graph tmp_graph;
  readGraph(tmp_graph); 
  
  std::map<int, Vertex> name_to_vertex;
  BGL_FORALL_VERTICES(v, tmp_graph, Graph)
    name_to_vertex[get(boost::vertex_name, tmp_graph, v)] = v;
  
  VertexNameMap vertex_to_name = get(boost::vertex_name, tmp_graph);
  
  unsigned num_of_nodes = boost::num_vertices(tmp_graph);

  std::unordered_map<unsigned, unsigned> address;
  initAddress(address);
  
  std::vector<string> instid(numTotalNodes, "");
  std::vector<string> dynamic_methodid(numTotalNodes, "");
  std::vector<string> prev_basic_block(numTotalNodes, "");
  
  initInstID(instid);
  initDynamicMethodID(dynamic_methodid);
  initPrevBasicBlock(prev_basic_block);
  
  std::vector<bool> tmp_isolated(num_of_nodes, 0);
  vertex_iter vi, vi_end;
  unsigned max_node_id = minNode;
  for (tie(vi, vi_end) = vertices(tmp_graph); vi != vi_end; ++vi)
  {
    unsigned node_id = vertex_to_name[*vi];
    if (boost::degree(*vi, tmp_graph) == 0)
      tmp_isolated.at(node_id) = 1;
    else
      if (node_id > max_node_id)
        max_node_id = node_id;
  }

  int shared_stores = 0;
  int node_id = max_node_id;
  auto loop_bound_it = loop_bound.end();
  loop_bound_it--;
  loop_bound_it--;
  while (node_id >=minNode )
  {
    //fprintf(stderr, "outter: node_id:%d,bound:%d\n", node_id, *loop_bound_it);
    unordered_map<unsigned, int> address_store_map;
    while (node_id >= *loop_bound_it && node_id >= minNode)
    {
      //fprintf(stderr, "inner: node_id:%d,bound:%d\n", node_id, *loop_bound_it);
      if (tmp_isolated.at(node_id) )
      {
        --node_id;
        continue;
      }
      int node_microop = microop.at(node_id);
      if (is_store_op(node_microop))
      {
        unsigned node_address = address[node_id];
        auto addr_it = address_store_map.find(node_address);
        if (addr_it == address_store_map.end())
          address_store_map[node_address] = node_id;
        else
        {
          //remove this store
          string store_unique_id (dynamic_methodid.at(node_id) + "-" + instid.at(node_id) + "-" + prev_basic_block.at(node_id));
          //dynamic stores, cannot disambiguated in the run time, cannot remove
          if (dynamicMemoryOps.find(store_unique_id) == dynamicMemoryOps.end())
          {
            Vertex node = name_to_vertex[node_id];
            //if it has children, ignore it
            if (boost::out_degree(node, tmp_graph)== 0)
            {
              //fprintf(stderr, "node_id:%d,bound:%d,node_address:%u,prev_store:%d\n", node_id, *loop_bound_it, node_address, *addr_it);
              microop.at(node_id) = LLVM_IR_SilentStore;
              shared_stores++;
            }
          }
        }
      }
      node_id--; 
    }
    if (node_id < *loop_bound_it)
    {
      if (loop_bound_it == loop_bound.begin())
        break;
      --loop_bound_it;
    }
  }
  cleanLeafNodes();
}

void Datapath::treeHeightReduction()
{
  std::cerr << "-------------------------------" << std::endl;
  std::cerr << "     Tree Height Reduction     " << std::endl;
  std::cerr << "-------------------------------" << std::endl;
  std::vector<int> loop_bound;
  string file_name(graphName);
  file_name += "_loop_bound";
  read_file(file_name, loop_bound);
  if (loop_bound.size() < 1)
    return;
  //set graph
  Graph tmp_graph;
  readGraph(tmp_graph); 
  
  unsigned num_of_nodes = boost::num_vertices(tmp_graph);
  unsigned num_of_edges = boost::num_edges(tmp_graph);
   
  VertexNameMap vertex_to_name = get(boost::vertex_name, tmp_graph);
  EdgeNameMap edge_to_name = get(boost::edge_name, tmp_graph);
  
  std::vector<bool> tmp_isolated(num_of_nodes, 0);
  std::vector<bool> updated(num_of_nodes, 0);
  std::vector<int> bound_region(num_of_nodes, 0);
  unsigned max_node_id = minNode;
  vertex_iter vi, vi_end;
  for (tie(vi, vi_end) = vertices(tmp_graph); vi != vi_end; ++vi)
  {
    unsigned node_id = vertex_to_name[*vi];
    if (boost::degree(*vi, tmp_graph) == 0)
    {
      tmp_isolated.at(node_id) = 1;
      updated.at(node_id) = 1;
    }
    else
      if (node_id > max_node_id)
        max_node_id = node_id;
  }

  int region_id = 0;
  int node_id = 0;
  auto b_it = loop_bound.begin();
  while (node_id < *b_it)
  {
    bound_region.at(node_id) = region_id;
    node_id++;
    if (node_id == *b_it)
    {
      region_id++;
      b_it++;
      if (b_it == loop_bound.end())
        break;
    }
  }
  
  std::vector<bool> to_remove_edges(num_of_edges, 0);
  std::vector<newEdge> to_add_edges;
  
  std::vector< Vertex > topo_nodes;
  boost::topological_sort(tmp_graph, std::back_inserter(topo_nodes));

  //nodes with no outgoing edges to first (bottom nodes first)
  for (auto vi = topo_nodes.begin(); vi != topo_nodes.end(); ++vi)
  {
    int node_id = vertex_to_name[*vi];
    if(tmp_isolated.at(node_id) || updated.at(node_id))
      continue;
    int node_microop = microop.at(node_id);
    if (!is_associative(node_microop))
      continue;
    updated.at(node_id) = 1;
    int node_region = bound_region.at(node_id); 
    //fprintf(stderr, "first node:%d,%d,%d\n", node_id, node_microop, node_region);
    std::list<int> nodes;
    std:;vector<int> tmp_remove_edges;
    std::vector<pair<int, bool> > leaves;
    
    std::vector< Vertex > associative_chain;
    associative_chain.push_back(*vi);
    //auto chain_it = associative_chain.begin();
    int chain_id = 0;
    //while (chain_it != associative_chain.end())
    while (chain_id < associative_chain.size())
    {
      Vertex chain_node = associative_chain.at(chain_id);
      //Vertex chain_node = *chain_it;
      int chain_node_id = vertex_to_name[chain_node];
      int chain_node_microop = microop.at(chain_node_id);
      int chain_node_region = bound_region.at(chain_node_id); 
      //fprintf(stderr, "checking: node:%d,%d,%d\n", chain_node_id, chain_node_microop, chain_node_region);
      if (is_associative(chain_node_microop))
      {
        nodes.push_front(chain_node_id);
        updated.at(node_id) = 1;
        //loop its parents
        in_edge_iter in_edge_it, in_edge_end;
        for (tie(in_edge_it, in_edge_end) = in_edges(chain_node , tmp_graph); in_edge_it != in_edge_end; ++in_edge_it)
        {
          Vertex parent_node = source(*in_edge_it, tmp_graph);
          int parent_id = vertex_to_name[parent_node];
          int parent_region = bound_region.at(parent_id);
          int parent_microop = microop.at(parent_id);
          int edge_id = edge_to_name[*in_edge_it];
          updated.at(parent_id) = 1;
          //fprintf(stderr, "node:%d,%d,%d\n", chain_node_id, chain_node_microop, chain_node_region);
          //fprintf(stderr, "parent:%d,%d,%d\n", parent_id, parent_microop, parent_region);
          if (parent_region == node_region)
          {
            if (is_associative(parent_microop) && boost::out_degree(parent_node , tmp_graph) == 1)
            {
              //to_remove_edges.at(edge_id) = 1;
              tmp_remove_edges.push_back(edge_id);
              associative_chain.push_back(parent_node);
              //fprintf(stderr, "pushing to the chain:%d\n", parent_id);
            }
            else
            {
              //to_remove_edges.at(edge_id) = 1;
              tmp_remove_edges.push_back(edge_id);
              leaves.push_back(make_pair(parent_id, 0));
            }
          }
          else
          {
            //to_remove_edges.at(edge_id) = 1;
            tmp_remove_edges.push_back(edge_id);
            leaves.push_back(make_pair(parent_id, 1));
          }
        }
      }
      else
        leaves.push_back(make_pair(chain_node_id, 0));
      chain_id++;
      //chain_it = associative_chain.erase(chain_it);
    }
    //fprintf(stderr, "DONE WITH FINDING NODES\n");
    //build the tree
    if (nodes.size() < 3)
      continue;
    for(auto it = tmp_remove_edges.begin(), E = tmp_remove_edges.end(); it != E; it++)
      to_remove_edges.at(*it) = 1;

    //fprintf(stderr, "foudn TREE\n");
    //fprintf(stderr, "num of leaves:%d\n", leaves.size());
    std::unordered_map<unsigned, unsigned> rank_map;
    auto leaf_it = leaves.begin();
    
    while (leaf_it != leaves.end())
    {
      //fprintf(stderr, "leaf node,%d, its rank,%d\n", leaf_it->first, leaf_it->second);
      if (leaf_it->second == 0)
        rank_map[leaf_it->first] = 0;
      else
        rank_map[leaf_it->first] = num_of_nodes;
      ++leaf_it; 
    }
    //fprintf(stderr, "after initializing rank map!\n");
    //reconstruct the rest of the balanced tree
    auto node_it = nodes.begin();
    
    while (node_it != nodes.end())
    {
      int node1, node2;
      if (rank_map.size() == 2)
      {
        node1 = rank_map.begin()->first;
        node2 = (++rank_map.begin())->first;
      }
      else
        findMinRankNodes(node1, node2, rank_map);
      //fprintf(stderr, "found nodes: %d,%d\n", node1, node2);
      assert((node1 != numTotalNodes) && (node2 != numTotalNodes));
      //fprintf(stderr, "adding edges between: %d,%d\n", node1, *node_it);
      //fprintf(stderr, "adding edges between: %d,%d\n", node2, *node_it);
      to_add_edges.push_back({node1, *node_it, 1, 1});
      to_add_edges.push_back({node2, *node_it, 1, 1});

      //place the new node in the map, remove the two old nodes
      rank_map[*node_it] = max(rank_map[node1], rank_map[node2]) + 1;
      rank_map.erase(node1);
      rank_map.erase(node2);
      ++node_it;
    }
    //fprintf(stderr, "DONE WITH RANKING_MAP\n");
  }
  //fprintf(stderr, "DONE WITH SCANNING\n");
  int curr_num_of_edges = writeGraphWithIsolatedEdges(to_remove_edges);
  writeGraphWithNewEdges(to_add_edges, curr_num_of_edges);

  cleanLeafNodes();
}
void Datapath::findMinRankNodes(int &node1, int &node2, std::unordered_map<unsigned, unsigned> &rank_map)
{
  //fprintf(stderr, "find min rank nodes\n");
  unsigned min_rank = numTotalNodes;
  for (auto it = rank_map.begin(); it != rank_map.end(); ++it)
  {
    //fprintf (stderr, "checking,%d,%d\n", it->first, it->second);
    int node_rank = it->second;
    if (node_rank < min_rank)
    {
      node1 = it->first;
      min_rank = node_rank;
    }
  }
  min_rank = numTotalNodes;
  for (auto it = rank_map.begin(); it != rank_map.end(); ++it)
  {
    //fprintf (stderr, "checking,%d,%d\n", it->first, it->second);
    int node_rank = it->second;
    if ((it->first != node1) && (node_rank < min_rank))
    {
      node2 = it->first;
      min_rank = node_rank;
    }
  }
  //fprintf(stderr, "node1:%d,node2:%d\n", node1, node2);
  //fprintf(stderr, "end of finding min rank nodes\n");
}
//readWriteGraph
int Datapath::writeGraphWithNewEdges(std::vector<newEdge> &to_add_edges, int curr_num_of_edges)
{
  string gn(graphName);
  string graph_file, edge_parid_file, edge_latency_file;
  graph_file = gn + "_graph";
  edge_parid_file = gn + "_edgeparid.gz";
  edge_latency_file = gn + "_edgelatency.gz";
  
  ifstream orig_graph;
  ofstream new_graph;
  gzFile new_edgelatency, new_edgeparid;
  
  orig_graph.open(graph_file.c_str());
  std::filebuf *pbuf = orig_graph.rdbuf();
  std::size_t size = pbuf->pubseekoff(0, orig_graph.end, orig_graph.in);
  pbuf->pubseekpos(0, orig_graph.in);
  char *buffer = new char[size];
  pbuf->sgetn (buffer, size);
  orig_graph.close();
  
  new_graph.open(graph_file.c_str());
  new_graph.write(buffer, size);
  delete[] buffer;

  long pos = new_graph.tellp();
  new_graph.seekp(pos-2);

  new_edgelatency = gzopen(edge_latency_file.c_str(), "a");
  new_edgeparid = gzopen(edge_parid_file.c_str(), "a");
  
  int new_edge_id = curr_num_of_edges;
  
  for(auto it = to_add_edges.begin(); it != to_add_edges.end(); ++it)
  {
    new_graph << it->from << " -> " 
              << it->to
              << " [e_id = " << new_edge_id << "];" << endl;
    new_edge_id++;
    gzprintf(new_edgelatency, "%d\n", it->latency);
    gzprintf(new_edgeparid, "%d\n", it->parid);
  }
  new_graph << "}" << endl;
  new_graph.close();
  gzclose(new_edgelatency);
  gzclose(new_edgeparid);

  return new_edge_id;
}
int Datapath::writeGraphWithIsolatedNodes(std::unordered_set<unsigned> &to_remove_nodes)
{
  Graph tmp_graph;
  readGraph(tmp_graph);
  
  unsigned num_of_edges = num_edges(tmp_graph);
  unsigned num_of_nodes = num_vertices(tmp_graph);
  
  std::vector<unsigned> edge_latency(num_of_edges, 0);
  std::vector<int> edge_parid(num_of_edges, 0);

  initEdgeLatency(edge_latency);
  initEdgeParID(edge_parid);
  
  VertexNameMap vertex_to_name = get(boost::vertex_name, tmp_graph);
  EdgeNameMap edge_to_name = get(boost::edge_name, tmp_graph);

  ofstream new_graph;
  gzFile new_edgelatency, new_edgeparid;
  
  string gn(graphName);
  string graph_file, edge_parid_file, edge_latency_file;
  graph_file = gn + "_graph";
  edge_parid_file = gn + "_edgeparid.gz";
  edge_latency_file = gn + "_edgelatency.gz";
  
  new_graph.open(graph_file.c_str());
  new_edgelatency = gzopen(edge_latency_file.c_str(), "a");
  new_edgeparid = gzopen(edge_parid_file.c_str(), "a");
  
  new_graph << "digraph DDDG {" << std::endl;

  for (unsigned node_id = 0; node_id < numTotalNodes; node_id++)
     new_graph << node_id << ";" << std::endl; 
  
  edge_iter ei, ei_end;
  int new_edge_id = 0;
  for (tie(ei, ei_end) = edges(tmp_graph); ei != ei_end; ++ei)
  {
    int edge_id = edge_to_name[*ei];
    int from = vertex_to_name[source(*ei, tmp_graph)];
    int to   = vertex_to_name[target(*ei, tmp_graph)];
    if (to_remove_nodes.find(from) != to_remove_nodes.end() 
     || to_remove_nodes.find(to) != to_remove_nodes.end())
      continue;

    new_graph << from << " -> " 
              << to
              << " [e_id = " << new_edge_id << "];" << endl;
    new_edge_id++;
    gzprintf(new_edgelatency, "%d\n", edge_latency.at(edge_id) );
    gzprintf(new_edgeparid, "%d\n", edge_parid.at(edge_id) );
  }

  new_graph << "}" << endl;
  new_graph.close();
  gzclose(new_edgelatency);
  gzclose(new_edgeparid);
  
  return new_edge_id;
}
int Datapath::writeGraphWithIsolatedEdges(std::vector<bool> &to_remove_edges)
{
  Graph tmp_graph;
  readGraph(tmp_graph);
  
  unsigned num_of_edges = num_edges(tmp_graph);
  unsigned num_of_nodes = num_vertices(tmp_graph);

  std::vector<unsigned> edge_latency(num_of_edges, 0);
  std::vector<int> edge_parid(num_of_edges, 0);

  initEdgeLatency(edge_latency);
  initEdgeParID(edge_parid);
  
  VertexNameMap vertex_to_name = get(boost::vertex_name, tmp_graph);
  EdgeNameMap edge_to_name = get(boost::edge_name, tmp_graph);

  ofstream new_graph;
  gzFile new_edgelatency, new_edgeparid;
  
  string gn(graphName);
  string graph_file, edge_parid_file, edge_latency_file;
  graph_file = gn + "_graph";
  edge_parid_file = gn + "_edgeparid.gz";
  edge_latency_file = gn + "_edgelatency.gz";
  
  new_graph.open(graph_file.c_str());
  new_edgelatency = gzopen(edge_latency_file.c_str(), "a");
  new_edgeparid = gzopen(edge_parid_file.c_str(), "a");

  new_graph << "digraph DDDG {" << std::endl;

  for (unsigned node_id = 0; node_id < num_of_nodes; node_id++)
     new_graph << node_id << ";" << std::endl; 
  
  edge_iter ei, ei_end;
  int new_edge_id = 0;
  for (tie(ei, ei_end) = edges(tmp_graph); ei != ei_end; ++ei)
  {
    int edge_id = edge_to_name[*ei];
    if (to_remove_edges.at(edge_id))
      continue;
    new_graph << vertex_to_name[source(*ei, tmp_graph)] << " -> " 
              << vertex_to_name[target(*ei, tmp_graph)] 
              << " [e_id = " << new_edge_id << "];" << endl;
    new_edge_id++;
    gzprintf(new_edgelatency, "%d\n", edge_latency.at(edge_id) );
    gzprintf(new_edgeparid, "%d\n", edge_parid.at(edge_id) );
  }
  
  new_graph << "}" << endl;
  new_graph.close();
  gzclose(new_edgelatency);
  gzclose(new_edgeparid);
  return new_edge_id;
}

void Datapath::readMethodGraph(MethodGraph &tmp_method_graph)
{
  string gn(graphName);
  string graph_file_name(gn + "_method_graph");
  
  boost::dynamic_properties dp;
  boost::property_map<MethodGraph, boost::vertex_name_t>::type v_name = get(boost::vertex_name, tmp_method_graph);
  boost::property_map<MethodGraph, boost::edge_name_t>::type e_name = get(boost::edge_name, tmp_method_graph);
  dp.property("n_id", v_name);
  dp.property("e_inst", e_name);
  std::ifstream fin(graph_file_name.c_str());
  boost::read_graphviz(fin, tmp_method_graph, dp, "n_id");
  
}


void Datapath::readGraph(Graph &tmp_graph)
{
  string gn(graphName);
  string graph_file_name(gn + "_graph");
  
  boost::dynamic_properties dp;
  boost::property_map<Graph, boost::vertex_name_t>::type v_name = get(boost::vertex_name, tmp_graph);
  boost::property_map<Graph, boost::edge_name_t>::type e_name = get(boost::edge_name, tmp_graph);
  dp.property("n_id", v_name);
  dp.property("e_id", e_name);
  std::ifstream fin(graph_file_name.c_str());
  boost::read_graphviz(fin, tmp_graph, dp, "n_id");

}

//initFunctions
void Datapath::writePerCycleActivity()
{
  string bn(benchName);
  //std::vector<int> mul_activity(cycle, 0);
  //std::vector<int> add_activity(cycle, 0);
  
  std::vector<string> dynamic_methodid(numTotalNodes, "");
  initDynamicMethodID(dynamic_methodid);
  
  std::unordered_map< string, std::vector<int> > mul_activity;
  std::unordered_map< string, std::vector<int> > add_activity;
  std::unordered_map< string, std::vector<int> > ld_activity;
  std::unordered_map< string, std::vector<int> > st_activity;
  
  std::vector<string> partition_names;
  scratchpad->partitionNames(partition_names);

  float avg_power, avg_fu_power, avg_mem_power, total_area, fu_area, mem_area;
  mem_area = 0;
  for (auto it = partition_names.begin(); it != partition_names.end() ; ++it)
  {
    string p_name = *it;
    std::vector<int> tmp_activity(cycle, 0);
    ld_activity[p_name] = tmp_activity;
    st_activity[p_name] = tmp_activity;
    mem_area += scratchpad->area(*it);
  }
  for (auto it = functionNames.begin(); it != functionNames.end() ; ++it)
  {
    string p_name = *it;
    std::vector<int> tmp_activity(cycle, 0);
    add_activity[p_name] = tmp_activity;
    mul_activity[p_name] = tmp_activity;
  }
  //cerr <<  "start activity" << endl;
  for(unsigned node_id = 0; node_id < numTotalNodes; ++node_id)
  {
    if (globalIsolated.at(node_id))
      continue;
    int tmp_level = newLevel.at(node_id);
    int node_microop = microop.at(node_id);
    if (node_microop == LLVM_IR_Mul || node_microop == LLVM_IR_UDiv)
    {
      char func_id[256];
      int count;
      sscanf(dynamic_methodid.at(node_id).c_str(), "%[^-]-%d\n", func_id, &count);
      mul_activity[func_id].at(tmp_level) +=1;
    }
    else if  (node_microop == LLVM_IR_Add || node_microop == LLVM_IR_Sub)
    {
      char func_id[256];
      int count;
      sscanf(dynamic_methodid.at(node_id).c_str(), "%[^-]-%d\n", func_id, &count);
      add_activity[func_id].at(tmp_level) +=1;
    }
    else if (is_load_op(node_microop))
    {
      string base_addr = baseAddress[node_id].first;
      ld_activity[base_addr].at(tmp_level) += 1;
    }
    else if (is_store_op(node_microop))
    {
      string base_addr = baseAddress[node_id].first;
      st_activity[base_addr].at(tmp_level) += 1;
    }
  }
  ofstream stats, power_stats;
  string tmp_name = bn + "_stats";
  stats.open(tmp_name.c_str());
  tmp_name += "_power";
  power_stats.open(tmp_name.c_str());

  stats << "cycles," << cycle << "," << numTotalNodes << endl; 
  power_stats << "cycles," << cycle << "," << numTotalNodes << endl; 
  stats << cycle << "," ;
  power_stats << cycle << "," ;
  
  int max_add =  0;
  int max_mul =  0;
  int max_reg_read =  0;
  int max_reg_write =  0;
  for (unsigned level_id = 0; ((int) level_id) < cycle; ++level_id)
  {
    if (max_reg_read < regStats.at(level_id).reads )
      max_reg_read = regStats.at(level_id).reads ;
    if (max_reg_write < regStats.at(level_id).writes )
      max_reg_write = regStats.at(level_id).writes ;
  }
  int max_reg = max_reg_read + max_reg_write;

  for (auto it = functionNames.begin(); it != functionNames.end() ; ++it)
  {
    stats << *it << "-mul," << *it << "-add,";
    power_stats << *it << "-mul," << *it << "-add,";
    max_add += *max_element(add_activity[*it].begin(), add_activity[*it].end());
    max_mul += *max_element(mul_activity[*it].begin(), mul_activity[*it].end());
  }

  //ADD_int_power, MUL_int_power, REG_int_power
  float add_leakage_per_cycle = ADD_leak_power * max_add;
  float mul_leakage_per_cycle = MUL_leak_power * max_mul;
  float reg_leakage_per_cycle = REG_leak_power * 32 * max_reg;

  fu_area = ADD_area * max_add + MUL_area * max_mul + REG_area * 32 * max_reg;
  total_area = mem_area + fu_area;
  
  for (auto it = partition_names.begin(); it != partition_names.end() ; ++it)
  {
    stats << *it << "," ;
    power_stats << *it << "," ;
  }
  stats << "reg" << endl;
  power_stats << "reg" << endl;

  avg_power = 0;
  avg_fu_power = 0;
  avg_mem_power = 0;
  
  for (unsigned tmp_level = 0; ((int)tmp_level) < cycle ; ++tmp_level)
  {
    stats << tmp_level << "," ;
    power_stats << tmp_level << ",";
    //For FUs
    for (auto it = functionNames.begin(); it != functionNames.end() ; ++it)
    {
      stats << mul_activity[*it].at(tmp_level) << "," << add_activity[*it].at(tmp_level) << ","; 
      float tmp_mul_power = (MUL_switch_power + MUL_int_power) * mul_activity[*it].at(tmp_level) + mul_leakage_per_cycle ;
      float tmp_add_power = (ADD_switch_power + ADD_int_power) * add_activity[*it].at(tmp_level) + add_leakage_per_cycle;
      avg_fu_power += tmp_mul_power + tmp_add_power;
      power_stats  << tmp_mul_power << "," << tmp_add_power << "," ;
    }
    //For memory
    for (auto it = partition_names.begin(); it != partition_names.end() ; ++it)
    {
      stats << ld_activity.at(*it).at(tmp_level) << "," << st_activity.at(*it).at(tmp_level) << "," ;
      float tmp_mem_power = scratchpad->readPower(*it) * ld_activity.at(*it).at(tmp_level) + 
                            scratchpad->writePower(*it) * st_activity.at(*it).at(tmp_level) + 
                            scratchpad->leakPower(*it);
      avg_mem_power += tmp_mem_power;
      power_stats << tmp_mem_power << "," ;
    }
    //For regs
    stats << regStats.at(tmp_level).reads << "," << regStats.at(tmp_level).writes <<endl;
    //reg power per bit
    float tmp_reg_power = (REG_int_power + REG_sw_power) *(regStats.at(tmp_level).reads + regStats.at(tmp_level).writes) * 32  + reg_leakage_per_cycle;
    avg_fu_power += tmp_reg_power;
    power_stats << tmp_reg_power << endl;
  }
  stats.close();
  power_stats.close();
  
  avg_fu_power /= cycle;
  avg_mem_power /= cycle;
  avg_power = avg_fu_power + avg_mem_power;
  //Summary output:
  //Cycle, Avg Power, Avg FU Power, Avg MEM Power, Total Area, FU Area, MEM Area
  std::cerr << "===============================" << std::endl;
  std::cerr << "        Aladdin Results        " << std::endl;
  std::cerr << "===============================" << std::endl;
  std::cerr << "Running : " << benchName << std::endl;
  std::cerr << "Cycle : " << cycle << " cycle" << std::endl;
  std::cerr << "Avg Power: " << avg_power << " mW" << std::endl;
  std::cerr << "Avg FU Power: " << avg_fu_power << " mW" << std::endl;
  std::cerr << "Avg MEM Power: " << avg_mem_power << " mW" << std::endl;
  std::cerr << "Total Area: " << total_area << " uM^2" << std::endl;
  std::cerr << "FU Area: " << fu_area << " uM^2" << std::endl;
  std::cerr << "MEM Area: " << mem_area << " uM^2" << std::endl;
  std::cerr << "===============================" << std::endl;
  std::cerr << "        Aladdin Results        " << std::endl;
  std::cerr << "===============================" << std::endl;
  
  ofstream summary;
  tmp_name = bn + "_summary";
  summary.open(tmp_name.c_str());
  summary << "===============================" << std::endl;
  summary << "        Aladdin Results        " << std::endl;
  summary << "===============================" << std::endl;
  summary << "Running : " << benchName << std::endl;
  summary << "Cycle : " << cycle << " cycle" << std::endl;
  summary << "Avg Power: " << avg_power << " mW" << std::endl;
  summary << "Avg FU Power: " << avg_fu_power << " mW" << std::endl;
  summary << "Avg MEM Power: " << avg_mem_power << " mW" << std::endl;
  summary << "Total Area: " << total_area << " uM^2" << std::endl;
  summary << "FU Area: " << fu_area << " uM^2" << std::endl;
  summary << "MEM Area: " << mem_area << " uM^2" << std::endl;
  summary << "===============================" << std::endl;
  summary << "        Aladdin Results        " << std::endl;
  summary << "===============================" << std::endl;
  summary.close();
}

void Datapath::writeGlobalIsolated()
{
  string file_name(benchName);
  file_name += "_global_isolated.gz";
  write_gzip_bool_file(file_name, globalIsolated.size(), globalIsolated);
}
void Datapath::writeFinalLevel()
{
  string file_name(benchName);
  file_name += "_level.gz";
  write_gzip_file(file_name, newLevel.size(), newLevel);
}
void Datapath::initMicroop(std::vector<int> &microop)
{
  string file_name(benchName);
  file_name += "_microop.gz";
  read_gzip_file(file_name, microop.size(), microop);
}
void Datapath::writeMicroop(std::vector<int> &microop)
{
  string file_name(benchName);
  file_name += "_microop.gz";
  write_gzip_file(file_name, microop.size(), microop);
}
void Datapath::initPrevBasicBlock(std::vector<string> &prevBasicBlock)
{
  string file_name(benchName);
  file_name += "_prevBasicBlock.gz";
  read_gzip_string_file(file_name, prevBasicBlock.size(), prevBasicBlock);
}
void Datapath::initDynamicMethodID(std::vector<string> &methodid)
{
  string file_name(benchName);
  file_name += "_dynamic_funcid.gz";
  read_gzip_string_file(file_name, methodid.size(), methodid);
}
void Datapath::initMethodID(std::vector<int> &methodid)
{
  string file_name(benchName);
  file_name += "_methodid.gz";
  read_gzip_file(file_name, methodid.size(), methodid);
}
void Datapath::initInstID(std::vector<std::string> &instid)
{
  string file_name(benchName);
  file_name += "_instid.gz";
  read_gzip_string_file(file_name, instid.size(), instid);
}
void Datapath::initAddress(std::unordered_map<unsigned, unsigned> &address)
{
  string file_name(benchName);
  file_name += "_memaddr.gz";
  gzFile gzip_file;
  gzip_file = gzopen(file_name.c_str(), "r");
  while (!gzeof(gzip_file))
  {
    char buffer[256];
    if (gzgets(gzip_file, buffer, 256) == NULL)
      break;
    unsigned node_id, addr, size;
    sscanf(buffer, "%d,%d,%d\n", &node_id, &addr, &size);
    address[node_id] = addr;
  }
  gzclose(gzip_file);
}
void Datapath::initAddressAndSize(std::unordered_map<unsigned, pair<unsigned, unsigned> > &address)
{
  string file_name(benchName);
  file_name += "_memaddr.gz";
  
  gzFile gzip_file;
  gzip_file = gzopen(file_name.c_str(), "r");
  while (!gzeof(gzip_file))
  {
    char buffer[256];
    if (gzgets(gzip_file, buffer, 256) == NULL)
      break;
    unsigned node_id, addr, size;
    sscanf(buffer, "%d,%d,%d\n", &node_id, &addr, &size);
    address[node_id] = make_pair(addr, size);
  }
  gzclose(gzip_file);
}

void Datapath::initializeGraphInMap(std::unordered_map<string, edgeAtt> &full_graph)
{
  Graph tmp_graph;
  readGraph(tmp_graph); 
  unsigned num_of_edges = boost::num_edges(tmp_graph);
  
  VertexNameMap vertex_to_name = get(boost::vertex_name, tmp_graph);
  EdgeNameMap edge_to_name = get(boost::edge_name, tmp_graph);
  
  std::vector<unsigned> edge_latency(num_of_edges, 0);
  std::vector<int> edge_parid(num_of_edges, 0);

  initEdgeLatency(edge_latency);
  initEdgeParID(edge_parid);
  
  //initialize full_graph
  edge_iter ei, ei_end;
  for (tie(ei, ei_end) = edges(tmp_graph); ei != ei_end; ++ei)
  {
    int edge_id = edge_to_name[*ei];
    int from = vertex_to_name[source(*ei, tmp_graph)];
    int to   = vertex_to_name[target(*ei, tmp_graph)];
    ostringstream oss;
    oss << from << "-" << to;
    full_graph[oss.str()] = {edge_parid.at(edge_id), edge_latency.at(edge_id)};
  }
}

void Datapath::writeGraphInMap(std::unordered_map<string, edgeAtt> &full_graph, string name)
{
  ofstream graph_file;
  gzFile new_edgelatency, new_edgeparid;
  
  string graph_name;
  string edge_parid_file, edge_latency_file;

  edge_parid_file = name + "_edgeparid.gz";
  edge_latency_file = name + "_edgelatency.gz";
  graph_name = name + "_graph";
  
  new_edgelatency = gzopen(edge_latency_file.c_str(), "w");
  new_edgeparid = gzopen(edge_parid_file.c_str(), "w");
  
  graph_file.open(graph_name.c_str());
  graph_file << "digraph DDDG {" << std::endl;
  for (unsigned node_id = 0; node_id < numTotalNodes; node_id++)
     graph_file << node_id << ";" << std::endl; 
  int new_edge_id = 0; 
  for(auto it = full_graph.begin(); it != full_graph.end(); it++)
  {
    int from, to;
    sscanf(it->first.c_str(), "%d-%d", &from, &to);
    
    graph_file << from << " -> " 
              << to
              << " [e_id = " << new_edge_id << "];" << endl;
    new_edge_id++;
    gzprintf(new_edgelatency, "%d\n", it->second.latency);
    gzprintf(new_edgeparid, "%d\n", it->second.parid);
  }
  graph_file << "}" << endl;
  graph_file.close();
  gzclose(new_edgelatency);
  gzclose(new_edgeparid);
}

void Datapath::initLineNum(std::vector<int> &line_num)
{
  ostringstream file_name;
  file_name << benchName << "_linenum.gz";
  read_gzip_file(file_name.str(), line_num.size(), line_num);
}
void Datapath::initGetElementPtr(std::unordered_map<unsigned, pair<string, unsigned> > &get_element_ptr)
{
  ostringstream file_name;
  file_name << benchName << "_getElementPtr.gz";
  gzFile gzip_file;
  gzip_file = gzopen(file_name.str().c_str(), "r");
  while (!gzeof(gzip_file))
  {
    char buffer[256];
    if (gzgets(gzip_file, buffer, 256) == NULL)
      break;
    unsigned node_id, address;
    char label[256];
    sscanf(buffer, "%d,%[^,],%d\n", &node_id, label, &address);
    get_element_ptr[node_id] = make_pair(label, address);
  }
  gzclose(gzip_file);
}
void Datapath::initEdgeParID(std::vector<int> &parid)
{
  string file_name(graphName);
  file_name += "_edgeparid.gz";
  read_gzip_file(file_name, parid.size(), parid);
}
void Datapath::initEdgeLatency(std::vector<unsigned> &edge_latency)
{
  string file_name(graphName);
  file_name += "_edgelatency.gz";
  read_gzip_unsigned_file(file_name, edge_latency.size(), edge_latency);
}
void Datapath::writeEdgeLatency(std::vector<unsigned> &edge_latency)
{
  string file_name(graphName);
  file_name += "_edgelatency.gz";
  write_gzip_unsigned_file(file_name, edge_latency.size(), edge_latency);
}



//stepFunctions
//multiple function, each function is a separate graph
void Datapath::setGraphForStepping(string graph_name)
{
  std::cerr << "=============================================" << std::endl;
  std::cerr << "      Scheduling...            " << graph_name << std::endl;
  std::cerr << "=============================================" << std::endl;
  graphName = (char*)graph_name.c_str();
  string gn(graphName);
  
  string graph_file_name(gn + "_graph");

  boost::dynamic_properties dp;
  boost::property_map<Graph, boost::vertex_name_t>::type v_name = get(boost::vertex_name, graph_);
  boost::property_map<Graph, boost::edge_name_t>::type e_name = get(boost::edge_name, graph_);
  dp.property("n_id", v_name);
  dp.property("e_id", e_name);
  std::ifstream fin(graph_file_name.c_str());
  boost::read_graphviz(fin, graph_, dp, "n_id");
  
  numGraphEdges = boost::num_edges(graph_);
  numGraphNodes = boost::num_vertices(graph_);
  
  vertexToName = get(boost::vertex_name, graph_);
  BGL_FORALL_VERTICES(v, graph_, Graph)
    nameToVertex[get(boost::vertex_name, graph_, v)] = v;
  
  edgeLatency.assign(numGraphEdges, 0);
  read_gzip_file(gn + "_edgelatency.gz", numGraphEdges, edgeLatency);
  
  isolated.assign(numGraphNodes, 0);
  numParents.assign(numGraphNodes, 0);
  
  totalConnectedNodes = 0;
  vertex_iter vi, vi_end;
  for (tie(vi, vi_end) = vertices(graph_); vi != vi_end; ++vi)
  {
    if (boost::degree(*vi, graph_) == 0)
      isolated.at(vertexToName[*vi]) = 1;
    else
    {
      numParents.at(vertexToName[*vi]) = boost::in_degree(*vi, graph_);
      totalConnectedNodes++;
    }
  }
  
  updateGlobalIsolated();

  executedNodes = 0;

  prevCycle = cycle;

  memReadyQueue.clear();
  nonMemReadyQueue.clear();
  executedQueue.clear();
  std::set<unsigned>().swap(memReadyQueue);
  std::set<unsigned>().swap(nonMemReadyQueue);
  std::vector<pair<unsigned,float> >().swap(executedQueue);

  initReadyQueue();
}

int Datapath::clearGraph()
{
  string gn(graphName);

  std::vector< Vertex > topo_nodes;
  boost::topological_sort(graph_, std::back_inserter(topo_nodes));
  //bottom nodes first
  std::vector<int> earliest_child(numGraphNodes, cycle);
  for (auto vi = topo_nodes.begin(); vi != topo_nodes.end(); ++vi)
  {
    int node_id = vertexToName[*vi];
    if (isolated.at(node_id))
      continue;
    unsigned node_microop = microop.at(node_id);
    if (!is_memory_op(node_microop) )
      //if (!is_memory_op(node_microop) && node_microop != LLVM_IR_Mul)
      if ((earliest_child.at(node_id) - 1 ) > newLevel.at(node_id))
        newLevel.at(node_id) = earliest_child.at(node_id) - 1;

    in_edge_iter in_i, in_end;
    for (tie(in_i, in_end) = in_edges(*vi , graph_); in_i != in_end; ++in_i)
    {
      int parent_id = vertexToName[source(*in_i, graph_)];
      if (earliest_child.at(parent_id) > newLevel.at(node_id))
        earliest_child.at(parent_id) = newLevel.at(node_id);
    }
  }
  earliest_child.clear();
  updateRegStats();
  edgeLatency.clear();
  //callLatency.clear();
  numParents.clear();
  isolated.clear();
  nameToVertex.clear();
  //vertexToName.clear();
  std::vector<int>().swap(edgeLatency);
  std::vector<bool>().swap(isolated);
  std::vector<int>().swap(numParents);
  
  graph_.clear();
  return cycle-prevCycle;
}
void Datapath::updateRegStats()
{
  std::map<int, Vertex> name_to_vertex;
  BGL_FORALL_VERTICES(v, graph_, Graph)
    name_to_vertex[get(boost::vertex_name, graph_, v)] = v;
  
  for(unsigned node_id = 0; node_id < numGraphNodes; node_id++)
  {
    if (isolated.at(node_id))
      continue;
    if (is_branch_op(microop.at(node_id)) || 
        is_index_op(microop.at(node_id)))
      continue;
    int node_level = newLevel.at(node_id);
    int max_children_level 		= node_level;
    
    Vertex node = name_to_vertex[node_id];
    out_edge_iter out_edge_it, out_edge_end;
    std::set<int> children_levels;
    for (tie(out_edge_it, out_edge_end) = out_edges(node, graph_); out_edge_it != out_edge_end; ++out_edge_it)
    {
      int child_id = vertexToName[target(*out_edge_it, graph_)];
      int node_microop = microop.at(child_id);
      if (is_branch_op(node_microop))
        continue;
      
      if (is_load_op(node_microop)) 
        continue;
      
      int child_level = newLevel.at(child_id);
      if (child_level > max_children_level)
        max_children_level = child_level;
      if (child_level > node_level  && child_level != cycle - 1)
        children_levels.insert(child_level);
        
    }
    for (auto it = children_levels.begin(); it != children_levels.end(); it++)
        regStats.at(*it).reads++;
    
    if (max_children_level > node_level )
      regStats.at(node_level).writes++;
    //for (int i = node_level; i < max_children_level; ++i)
      //regStats.at(i).size++;
  }
}
bool Datapath::step()
{
//#ifdef DEBUG
  //cerr << "===========Stepping============" << endl;
//#endif
  int firedNodes = fireNonMemNodes();
  firedNodes += fireMemNodes();

  stepExecutedQueue();

  cycle++;
  if (executedNodes == totalConnectedNodes)
    return 1;
  return 0;
}

void Datapath::stepExecutedQueue()
{
  auto it = executedQueue.begin();
  while (it != executedQueue.end())
  {
    //it->second is the number of cycles to execute current nodes
    if (it->second <= cycleTime)
    {
      unsigned node_id = it->first;
      executedNodes++;
      newLevel.at(node_id) = cycle;
      it = executedQueue.erase(it);
    }
    else
    {
      it->second -= cycleTime;
      it++;
    }
  }
}
void Datapath::updateChildren(unsigned node_id, float latencySoFar)
{
  Vertex node = nameToVertex[node_id];
  out_edge_iter out_edge_it, out_edge_end;
  for (tie(out_edge_it, out_edge_end) = out_edges(node, graph_); out_edge_it != out_edge_end; ++out_edge_it)
  {
    int child_id = vertexToName[target(*out_edge_it, graph_)];
    if (numParents[child_id] > 0)
    {
      numParents[child_id]--;
      if (numParents[child_id] == 0)
      {
        if (is_memory_op(microop.at(child_id)))
          addMemReadyNode(child_id);
        else
        {
          executedQueue.push_back(make_pair(child_id, latencySoFar + node_latency(microop.at(child_id))));
          updateChildren(child_id, latencySoFar + node_latency(microop.at(child_id)));
        }
      }
    }
  }
}
int Datapath::fireMemNodes()
{
  int firedNodes = 0;
  auto it = memReadyQueue.begin();
  while (it != memReadyQueue.end() && scratchpad->canService())
  {
    unsigned node_id = *it;
    //need to check scratchpad constraints
    string node_part = baseAddress[node_id].first;
    //fprintf(stderr, "nodeid,%d,node-part,%s\n", node_id, node_part.c_str());
    if(scratchpad->canServicePartition(node_part))
    {
      assert(scratchpad->addressRequest(node_part));
      //assign levels and add to executed queue
      executedQueue.push_back(make_pair(node_id, node_latency(microop.at(node_id))));
      updateChildren(node_id, node_latency(microop.at(node_id)));
      it = memReadyQueue.erase(it);
      firedNodes++;
    }
    else
      ++it;
  }
  return firedNodes;
}

int Datapath::fireNonMemNodes()
{
  int firedNodes = 0;
  //assume the Queue is sorted by somehow
  auto it = nonMemReadyQueue.begin();
  while (it != nonMemReadyQueue.end())
  {
    unsigned node_id = *it;
    firedNodes++;
    executedQueue.push_back(make_pair(node_id, node_latency(microop.at(node_id))));
    updateChildren(node_id, node_latency(microop.at(node_id)));
    it = nonMemReadyQueue.erase(it);
  }
  return firedNodes;
}

void Datapath::initReadyQueue()
{
  for(unsigned i = 0; i < numGraphNodes; i++)
  {
    if (numParents[i] == 0 && isolated[i] != 1)
    {
      if (is_memory_op(microop.at(i)))
        addMemReadyNode(i);
      else
        addNonMemReadyNode(i);
    }
  }
}

void Datapath::addMemReadyNode(unsigned node_id)
{
  memReadyQueue.insert(node_id);
}

void Datapath::addNonMemReadyNode(unsigned node_id)
{
  nonMemReadyQueue.insert(node_id);
}

void Datapath::updateGlobalIsolated()
{
  for (unsigned node_id = 0; node_id < numGraphNodes; ++node_id)
  {
    if (isolated.at(node_id) == 0)
      globalIsolated.at(node_id) = 0;
  }
}

//readConfigs
bool Datapath::readUnrollingConfig(std::unordered_map<int, int > &unrolling_config)
{
  ifstream config_file;
  string file_name(benchName);
  file_name += "_unrolling_config";
  config_file.open(file_name.c_str());
  if (!config_file.is_open())
    return 0;
  while(!config_file.eof())
  {
    string wholeline;
    getline(config_file, wholeline);
    if (wholeline.size() == 0)
      break;
    char func[256];
    int line_num, factor;
    sscanf(wholeline.c_str(), "%[^,],%d,%d\n", func, &line_num, &factor);
    unrolling_config[line_num] =factor;
  }
  config_file.close();
  return 1;
}

bool Datapath::readFlattenConfig(std::unordered_set<int> &flatten_config)
{
  ifstream config_file;
  string file_name(benchName);
  file_name += "_flatten_config";
  config_file.open(file_name.c_str());
  if (!config_file.is_open())
    return 0;
  while(!config_file.eof())
  {
    string wholeline;
    getline(config_file, wholeline);
    if (wholeline.size() == 0)
      break;
    char func[256];
    int line_num;
    sscanf(wholeline.c_str(), "%[^,],%d\n", func, &line_num);
    flatten_config.insert(line_num);
  }
  config_file.close();
  return 1;
}


bool Datapath::readCompletePartitionConfig(std::unordered_set<string> &config)
{
  string bn(benchName);
  string comp_partition_file;
  comp_partition_file = bn + "_complete_partition_config";
  
  if (!fileExists(comp_partition_file))
    return 0;
  
  ifstream config_file;
  config_file.open(comp_partition_file);
  string wholeline;
  while(!config_file.eof())
  {
    getline(config_file, wholeline);
    if (wholeline.size() == 0)
      break;
    unsigned size;
    char type[256];
    char base_addr[256];
    sscanf(wholeline.c_str(), "%[^,],%[^,],%d\n", type, base_addr, &size);
    config.insert(base_addr);
  }
  config_file.close();
  return 1;
}

bool Datapath::readPartitionConfig(std::unordered_map<string, partitionEntry> & partition_config)
{
  ifstream config_file;
  string file_name(benchName);
  file_name += "_partition_config";
  if (!fileExists(file_name))
    return 0;

  config_file.open(file_name.c_str());
  string wholeline;
  while (!config_file.eof())
  {
    getline(config_file, wholeline);
    if (wholeline.size() == 0) break;
    unsigned size, p_factor;
    char type[256];
    char base_addr[256];
    sscanf(wholeline.c_str(), "%[^,],%[^,],%d,%d,\n", type, base_addr, &size, &p_factor);
    string p_type(type);
    partition_config[base_addr] = {p_type, size, p_factor};
  }
  config_file.close();
  return 1;
}