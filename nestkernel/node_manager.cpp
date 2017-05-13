/*
 *  node_manager.cpp
 *
 *  This file is part of NEST.
 *
 *  Copyright (C) 2004 The NEST Initiative
 *
 *  NEST is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  NEST is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with NEST.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "node_manager.h"

// C++ includes:
#include <set>

// Includes from libnestutil:
#include "compose.hpp"
#include "logging.h"

// Includes from nestkernel:
#include "event_delivery_manager.h"
#include "genericmodel.h"
#include "genericmodel_impl.h"
#include "kernel_manager.h"
#include "model.h"
#include "model_manager_impl.h"
#include "node.h"
#include "sibling_container.h"
#include "vp_manager.h"
#include "vp_manager_impl.h"

// Includes from sli:
#include "dictutils.h"

namespace nest
{

NodeManager::NodeManager()
  : local_nodes_()
  , siblingcontainer_model_( 0 )
  , n_gsd_( 0 )
  , nodes_vec_()
  , wfr_nodes_vec_()
  , wfr_is_used_( false )
  , nodes_vec_network_size_( 0 ) // zero to force update
  , num_active_nodes_( 0 )
{
}

NodeManager::~NodeManager()
{
  destruct_nodes_(); // We must destruct nodes properly, since devices may need
                     // to close files.
}

void
NodeManager::initialize()
{
  siblingcontainer_model_ = kernel().model_manager.get_model( 0 );
  assert( siblingcontainer_model_ != 0 );
  assert( siblingcontainer_model_->get_name() == "siblingcontainer" );

  // explicitly force construction of nodes_vec_ to ensure consistent state
  nodes_vec_network_size_ = 0;
  ensure_valid_thread_local_ids();
}

void
NodeManager::finalize()
{
  destruct_nodes_();
}

void
NodeManager::reinit_nodes()
{
  /* Reinitialize state on all nodes, force init_buffers() on next
      call to simulate().
      Finding all nodes is non-trivial:
      - We iterate over local nodes only.
      - Nodes without proxies are not registered in nodes_. Instead, a
        SiblingContainer is created as container, and this container is
        stored in nodes_. The container then contains the actual nodes,
        which need to be reset.
      Thus, we iterate nodes_; additionally, we iterate the content of
      a Node if it's model id is -1, which indicates that it is a
      container.  Subnets are not iterated, since their nodes are
      registered in nodes_ directly.
    */
  for ( size_t n = 0; n < local_nodes_.size(); ++n )
  {
    Node* node = local_nodes_.get_node_by_index( n );
    assert( node != 0 );
    if ( node->num_thread_siblings() == 0 ) // not a SiblingContainer
    {
      node->init_state();
      node->set_buffers_initialized( false );
    }
    else if ( node->get_model_id() == -1 )
    {
      SiblingContainer* const c = dynamic_cast< SiblingContainer* >( node );
      assert( c );
      for ( std::vector< Node* >::iterator it = c->begin(); it != c->end();
            ++it )
      {
        ( *it )->init_state();
        ( *it )->set_buffers_initialized( false );
      }
    }
  }
}

DictionaryDatum
NodeManager::get_status( index idx )
{
  Node* target = get_node( idx );
  assert( target != 0 );

  DictionaryDatum d = target->get_status_base();

  return d;
}

GIDCollectionPTR
NodeManager::add_node( index mod, long n )
{
  if ( mod >= kernel().model_manager.get_num_node_models() )
  {
    throw UnknownModelID( mod );
  }

  if ( n < 1 )
  {
    throw BadProperty();
  }

  const thread n_threads = kernel().vp_manager.get_num_threads();
  assert( n_threads > 0 );

  const index min_gid = local_nodes_.get_max_gid() + 1;
  const index max_gid = min_gid + n;

  Model* model = kernel().model_manager.get_model( mod );
  assert( model != 0 );

  model->deprecation_warning( "Create" );

  if ( max_gid > local_nodes_.max_size() || max_gid < min_gid )
  {
    LOG( M_ERROR,
      "NodeManager::add_node",
      "Requested number of nodes will overflow the memory." );
    LOG( M_ERROR, "NodeManager::add:node", "No nodes were created." );
    throw KernelException( "OutOfMemory" );
  }
  kernel().modelrange_manager.add_range( mod, min_gid, max_gid - 1 );

  if ( model->potential_global_receiver()
    and kernel().mpi_manager.get_num_rec_processes() > 0 )
  {
    // In this branch we create nodes for global receivers
    const int n_per_process = n / kernel().mpi_manager.get_num_rec_processes();
    const int n_per_thread = n_per_process / n_threads + 1;

    // We only need to reserve memory on the ranks on which we
    // actually create nodes. In this if-branch ---> Only on recording
    // processes
    if ( kernel().mpi_manager.get_rank()
      >= kernel().mpi_manager.get_num_sim_processes() )
    {
      local_nodes_.reserve( std::ceil( static_cast< double >( max_gid )
        / kernel().mpi_manager.get_num_sim_processes() ) );
      for ( thread t = 0; t < n_threads; ++t )
      {
        // Model::reserve() reserves memory for n ADDITIONAL nodes on thread t
        model->reserve_additional( t, n_per_thread );
      }
    }

    for ( size_t gid = min_gid; gid < max_gid; ++gid )
    {
      const thread vp = kernel().vp_manager.suggest_rec_vp( get_n_gsd() );
      const thread t = kernel().vp_manager.vp_to_thread( vp );

      if ( kernel().vp_manager.is_local_vp( vp ) )
      {
        Node* newnode = model->allocate( t );
        newnode->set_gid_( gid );
        newnode->set_model_id( mod );
        newnode->set_thread( t );
        newnode->set_vp( vp );
        newnode->set_has_proxies( true );
        newnode->set_local_receiver( false );

        local_nodes_.add_local_node( *newnode ); // put into local nodes list
      }
      else
      {
        local_nodes_.add_remote_node( gid ); // ensures max_gid is correct
      }
      increment_n_gsd();
    }
  }

  else if ( model->has_proxies() )
  {
    // In this branch we create nodes for all GIDs which are on a local thread
    const int n_per_process = n / kernel().mpi_manager.get_num_sim_processes();
    const int n_per_thread = n_per_process / n_threads + 1;

    // We only need to reserve memory on the ranks on which we
    // actually create nodes. In this if-branch ---> Only on
    // simulation processes
    if ( kernel().mpi_manager.get_rank()
      < kernel().mpi_manager.get_num_sim_processes() )
    {
      // TODO: This will work reasonably for round-robin. The extra 50 entries
      //       are for devices.
      local_nodes_.reserve(
        std::ceil( static_cast< double >( max_gid )
          / kernel().mpi_manager.get_num_sim_processes() ) + 50 );
      for ( thread t = 0; t < n_threads; ++t )
      {
        // Model::reserve() reserves memory for n ADDITIONAL nodes on thread t
        // reserves at least one entry on each thread, nobody knows why
        model->reserve_additional( t, n_per_thread );
      }
    }

    size_t gid;
    if ( kernel().vp_manager.is_local_vp(
           kernel().vp_manager.suggest_vp( min_gid ) ) )
    {
      gid = min_gid;
    }
    else
    {
      gid = next_local_gid_( min_gid );
    }

    // min_gid is first valid gid i should create, hence ask for the first local
    // gid after min_gid-1
    while ( gid < max_gid )
    {
      const thread vp = kernel().vp_manager.suggest_vp( gid );
      const thread t = kernel().vp_manager.vp_to_thread( vp );

      if ( kernel().vp_manager.is_local_vp( vp ) )
      {
        Node* newnode = model->allocate( t );
        newnode->set_gid_( gid );
        newnode->set_model_id( mod );
        newnode->set_thread( t );
        newnode->set_vp( vp );

        local_nodes_.add_local_node( *newnode ); // put into local nodes list

        gid = next_local_gid_( gid );
      }
      else
      {
        ++gid;
      }
    }

    // if last gid is not on this process, we need to add it as a remote node
    if ( not kernel().vp_manager.is_local_vp(
           kernel().vp_manager.suggest_vp( max_gid - 1 ) ) )
    {
      local_nodes_.add_remote_node( max_gid - 1 ); // ensures max_gid is correct
    }
  }
  else if ( not model->one_node_per_process() )
  {
    // We allocate space for n containers which will hold the threads
    // sorted. We use SiblingContainers to store the instances for
    // each thread to exploit the very efficient memory allocation for
    // nodes.
    //
    // These containers are registered in the global nodes_ array to
    // provide access to the instances both for manipulation by SLI
    // functions and so that NodeManager::calibrate() can discover the
    // instances and register them for updating.
    //
    // The instances are also registered with the instance of the
    // current subnet for the thread to which the created instance
    // belongs. This is mainly important so that the subnet structure
    // is preserved on all VPs.  Node enumeration is done on by the
    // registration with the per-thread instances.
    //
    // The wrapper container can be addressed under the GID assigned
    // to no-proxy node created. If this no-proxy node is NOT a
    // container (e.g. a device), then each instance can be retrieved
    // by giving the respective thread-id to get_node(). Instances of
    // SiblingContainers cannot be addressed individually.
    //
    // The allocation of the wrapper containers is spread over threads
    // to balance memory load.
    size_t container_per_thread = n / n_threads + 1;

    // since we create the n nodes on each thread, we reserve the full load.
    for ( thread t = 0; t < n_threads; ++t )
    {
      model->reserve_additional( t, n );
      siblingcontainer_model_->reserve_additional( t, container_per_thread );
    }

    // The following loop creates n nodes. For each node, a wrapper is created
    // and filled with one instance per thread, in total n * n_thread nodes in
    // n wrappers.
    local_nodes_.reserve(
      std::ceil( static_cast< double >( max_gid )
        / kernel().mpi_manager.get_num_sim_processes() ) + 50 );
    for ( index gid = min_gid; gid < max_gid; ++gid )
    {
      thread thread_id = kernel().vp_manager.vp_to_thread(
        kernel().vp_manager.suggest_vp( gid ) );

      // Create wrapper and register with nodes_ array.
      SiblingContainer* container = static_cast< SiblingContainer* >(
        siblingcontainer_model_->allocate( thread_id ) );
      container->set_model_id(
        -1 ); // mark as pseudo-container wrapping replicas, see reset_network()
      container->reserve( n_threads ); // space for one instance per thread
      container->set_gid_( gid );
      local_nodes_.add_local_node( *container );

      // Generate one instance of desired model per thread
      for ( thread t = 0; t < n_threads; ++t )
      {
        Node* newnode = model->allocate( t );
        newnode->set_gid_( gid ); // all instances get the same global id.
        newnode->set_model_id( mod );
        newnode->set_thread( t );
        newnode->set_vp( kernel().vp_manager.thread_to_vp( t ) );

        // Register instance with wrapper
        // container has one entry for each thread
        container->push_back( newnode );
      }
    }
  }
  else
  {
    // no proxies and one node per process
    // this is used by MUSIC proxies
    // Per r9700, this case is only relevant for music_*_proxy models,
    // which have a single instance per MPI process.
    for ( index gid = min_gid; gid < max_gid; ++gid )
    {
      Node* newnode = model->allocate( 0 );
      newnode->set_gid_( gid );
      newnode->set_model_id( mod );
      newnode->set_thread( 0 );
      newnode->set_vp( kernel().vp_manager.thread_to_vp( 0 ) );

      // Register instance
      local_nodes_.add_local_node( *newnode );
    }
  }

  // set off-grid spike communication if necessary
  if ( model->is_off_grid() )
  {
    kernel().event_delivery_manager.set_off_grid_communication( true );
    LOG( M_INFO,
      "NodeManager::add_node",
      "Neuron models emitting precisely timed spikes exist: "
      "the kernel property off_grid_spiking has been set to true.\n\n"
      "NOTE: Mixing precise-spiking and normal neuron models may "
      "lead to inconsistent results." );
  }

  return GIDCollectionPTR(
    new GIDCollectionPrimitive( min_gid, max_gid - 1, mod ) );
}

void
NodeManager::restore_nodes( const ArrayDatum& node_list )
{
  Token* first = node_list.begin();
  const Token* end = node_list.end();
  if ( first == end )
  {
    return;
  }

  for ( Token* node_t = first; node_t != end; ++node_t )
  {
    DictionaryDatum node_props = getValue< DictionaryDatum >( *node_t );
    std::string model_name = ( *node_props )[ names::model ];
    index model_id = kernel().model_manager.get_model_id( model_name.c_str() );
    GIDCollectionPTR node = add_node( model_id );
    Node* node_ptr = get_node( ( *node->begin() ).gid );
    // we call directly set_status on the node
    // to bypass checking of unused dictionary items.
    node_ptr->set_status_base( node_props );
  }
}

void
NodeManager::init_state( index GID )
{
  Node* n = get_node( GID );
  if ( n == 0 )
  {
    throw UnknownNode( GID );
  }

  n->init_state();
}

bool
NodeManager::is_local_node( Node* n ) const
{
  return kernel().vp_manager.is_local_vp( n->get_vp() );
}

inline index
NodeManager::next_local_gid_( index curr_gid ) const
{
  index rank = kernel().mpi_manager.get_rank();
  index sim_procs = kernel().mpi_manager.get_num_sim_processes();
  if ( rank >= sim_procs )
  {
    // i am a rec proc trying to add a non-gsd node => just iterate to next gid
    return curr_gid + sim_procs;
  }
  // responsible process for curr_gid
  index proc_of_curr_gid = curr_gid % sim_procs;

  if ( proc_of_curr_gid == rank )
  {
    // I am responsible for curr_gid, then add 'modulo'.
    return curr_gid + sim_procs;
  }
  else
  {
    // else add difference
    // make modulo positive and difference of my proc an curr_gid proc
    return curr_gid + ( sim_procs + rank - proc_of_curr_gid ) % sim_procs;
  }
}

Node* NodeManager::get_node( index n, thread thr ) // no_p
{
  Node* node = local_nodes_.get_node_by_gid( n );
  if ( node == 0 )
  {
    return kernel().model_manager.get_proxy_node( thr, n );
  }

  if ( node->num_thread_siblings() == 0 )
  {
    return node; // plain node
  }

  if ( thr < 0 || thr >= static_cast< thread >( node->num_thread_siblings() ) )
  {
    throw UnknownNode();
  }

  return node->get_thread_sibling( thr );
}

const SiblingContainer*
NodeManager::get_thread_siblings( index n ) const
{
  Node* node = local_nodes_.get_node_by_gid( n );
  if ( node->num_thread_siblings() == 0 )
  {
    throw NoThreadSiblingsAvailable( n );
  }
  const SiblingContainer* siblings = dynamic_cast< SiblingContainer* >( node );
  assert( siblings != 0 );

  return siblings;
}

void
NodeManager::ensure_valid_thread_local_ids()
{
  // Check if the network size changed, in order to not enter
  // the critical region if it is not necessary. Note that this
  // test also covers that case that nodes have been deleted
  // by reset.
  if ( size() == nodes_vec_network_size_ )
  {
    return;
  }

#ifdef _OPENMP
#pragma omp critical( update_nodes_vec )
  {
// This code may be called from a thread-parallel context, when it is
// invoked by TargetIdentifierIndex::set_target() during parallel
// wiring. Nested OpenMP parallelism is problematic, therefore, we
// enforce single threading here. This should be unproblematic wrt
// performance, because the nodes_vec_ is rebuilt only once after
// changes in network size.
#endif

    // Check again, if the network size changed, since a previous thread
    // can have updated nodes_vec_ before.
    if ( size() != nodes_vec_network_size_ )
    {

      /* We clear the existing nodes_vec_ and then rebuild it. */
      nodes_vec_.clear();
      nodes_vec_.resize( kernel().vp_manager.get_num_threads() );
      wfr_nodes_vec_.clear();
      wfr_nodes_vec_.resize( kernel().vp_manager.get_num_threads() );

      for ( index t = 0; t < kernel().vp_manager.get_num_threads(); ++t )
      {
        nodes_vec_[ t ].clear();
        wfr_nodes_vec_[ t ].clear();

        // Loops below run from index 1, because index 0 is always the root
        // network, which is never updated.
        size_t num_thread_local_nodes = 0;
        size_t num_thread_local_wfr_nodes = 0;
        for ( size_t idx = 0; idx < local_nodes_.size(); ++idx )
        {
          Node* node = local_nodes_.get_node_by_index( idx );
          if ( static_cast< index >( node->get_thread() ) == t
            or node->num_thread_siblings() > 0 )
          {
            num_thread_local_nodes++;
            if ( node->node_uses_wfr() )
            {
              num_thread_local_wfr_nodes++;
            }
          }
        }
        nodes_vec_[ t ].reserve( num_thread_local_nodes );
        wfr_nodes_vec_[ t ].reserve( num_thread_local_wfr_nodes );

        for ( size_t idx = 0; idx < local_nodes_.size(); ++idx )
        {
          Node* node = local_nodes_.get_node_by_index( idx );

          // If a node has thread siblings, it is a sibling container, and we
          // need to add the replica for the current thread. Otherwise, we have
          // a normal node, which is added only on the thread it belongs to.
          if ( node->num_thread_siblings() > 0 )
          {
            node->get_thread_sibling( t )->set_thread_lid(
              nodes_vec_[ t ].size() );
            nodes_vec_[ t ].push_back( node->get_thread_sibling( t ) );
          }
          else if ( static_cast< index >( node->get_thread() ) == t )
          {
            // these nodes cannot be subnets
            node->set_thread_lid( nodes_vec_[ t ].size() );
            nodes_vec_[ t ].push_back( node );

            if ( node->node_uses_wfr() )
            {
              wfr_nodes_vec_[ t ].push_back( node );
            }
          }
        }
      } // end of for threads

      nodes_vec_network_size_ = size();

      wfr_is_used_ = false;
      // wfr_is_used_ indicates, whether at least one
      // of the threads has a neuron that uses waveform relaxtion
      // all threads then need to perform a wfr_update
      // step, because gather_events() has to be done in a
      // openmp single section
      for ( index t = 0; t < kernel().vp_manager.get_num_threads(); ++t )
      {
        if ( wfr_nodes_vec_[ t ].size() > 0 )
        {
          wfr_is_used_ = true;
        }
      }
    }
#ifdef _OPENMP
  } // end of omp critical region
#endif
}

void
NodeManager::destruct_nodes_()
{
  // We call the destructor for each node excplicitly. This destroys
  // the objects without releasing their memory. Since the Memory is
  // owned by the Model objects, we must not call delete on the Node
  // objects!
  for ( size_t n = 0; n < local_nodes_.size(); ++n )
  {
    Node* node = local_nodes_.get_node_by_index( n );
    assert( node != 0 );
    for ( size_t t = 0; t < node->num_thread_siblings(); ++t )
    {
      node->get_thread_sibling( t )->~Node();
    }
    node->~Node();
  }

  local_nodes_.clear();
}

void
NodeManager::set_status_single_node_( Node& target,
  const DictionaryDatum& d,
  bool clear_flags )
{
  // proxies have no properties
  if ( not target.is_proxy() )
  {
    if ( clear_flags )
    {
      d->clear_access_flags();
    }
    target.set_status_base( d );

    // TODO: Not sure this check should be at single neuron level; advantage is
    // it stops after first failure.
    ALL_ENTRIES_ACCESSED(
      *d, "NodeManager::set_status", "Unread dictionary entries: " );
  }
}

void
NodeManager::prepare_node_( Node* n )
{
  // Frozen nodes are initialized and calibrated, so that they
  // have ring buffers and can accept incoming spikes.
  n->init_buffers();
  n->calibrate();
}

void
NodeManager::prepare_nodes()
{
  assert( kernel().is_initialized() );

  /* We initialize the buffers of each node and calibrate it. */

  size_t num_active_nodes = 0;     // counts nodes that will be updated
  size_t num_active_wfr_nodes = 0; // counts nodes that use waveform relaxation

  std::vector< lockPTR< WrappedThreadException > > exceptions_raised(
    kernel().vp_manager.get_num_threads() );

#ifdef _OPENMP
#pragma omp parallel reduction( + : num_active_nodes, num_active_wfr_nodes )
  {
    size_t t = kernel().vp_manager.get_thread_id();
#else
    for ( index t = 0; t < kernel().vp_manager.get_num_threads(); ++t )
    {
#endif

    // We prepare nodes in a parallel region. Therefore, we need to catch
    // exceptions here and then handle them after the parallel region.
    try
    {
      for ( std::vector< Node* >::iterator it = nodes_vec_[ t ].begin();
            it != nodes_vec_[ t ].end();
            ++it )
      {
        prepare_node_( *it );
        if ( not( *it )->is_frozen() )
        {
          ++num_active_nodes;
          if ( ( *it )->node_uses_wfr() )
          {
            ++num_active_wfr_nodes;
          }
        }
      }
    }
    catch ( std::exception& e )
    {
      // so throw the exception after parallel region
      exceptions_raised.at( t ) =
        lockPTR< WrappedThreadException >( new WrappedThreadException( e ) );
    }

  } // end of parallel section / end of for threads

  // check if any exceptions have been raised
  for ( index thr = 0; thr < kernel().vp_manager.get_num_threads(); ++thr )
  {
    if ( exceptions_raised.at( thr ).valid() )
    {
      throw WrappedThreadException( *( exceptions_raised.at( thr ) ) );
    }
  }

  std::ostringstream os;
  std::string tmp_str = num_active_nodes == 1 ? " node" : " nodes";
  os << "Preparing " << num_active_nodes << tmp_str << " for simulation.";

  if ( num_active_wfr_nodes != 0 )
  {
    tmp_str = num_active_wfr_nodes == 1 ? " uses " : " use ";
    os << " " << num_active_wfr_nodes << " of them" << tmp_str
       << "iterative solution techniques.";
  }

  num_active_nodes_ = num_active_nodes;
  LOG( M_INFO, "NodeManager::prepare_nodes", os.str() );
}

void
NodeManager::post_run_cleanup()
{
#ifdef _OPENMP
#pragma omp parallel
  {
    index t = kernel().vp_manager.get_thread_id();
#else // clang-format off
  for ( index t = 0; t < kernel().vp_manager.get_num_threads(); ++t )
  {
#endif // clang-format on
    for ( size_t idx = 0; idx < local_nodes_.size(); ++idx )
    {
      Node* node = local_nodes_.get_node_by_index( idx );
      if ( node != 0 )
      {
        if ( node->num_thread_siblings() > 0 )
        {
          node->get_thread_sibling( t )->post_run_cleanup();
        }
        else
        {
          if ( static_cast< index >( node->get_thread() ) == t )
          {
            node->post_run_cleanup();
          }
        }
      }
    }
  }
}

/**
 * This function is called only if the thread data structures are properly set
 * up.
 */
void
NodeManager::finalize_nodes()
{
#ifdef _OPENMP
#pragma omp parallel
  {
    index t = kernel().vp_manager.get_thread_id();
#else // clang-format off
  for ( index t = 0; t < kernel().vp_manager.get_num_threads(); ++t )
  {
#endif // clang-format on
    for ( size_t idx = 0; idx < local_nodes_.size(); ++idx )
    {
      Node* node = local_nodes_.get_node_by_index( idx );
      if ( node != 0 )
      {
        if ( node->num_thread_siblings() > 0 )
        {
          node->get_thread_sibling( t )->finalize();
        }
        else
        {
          if ( static_cast< index >( node->get_thread() ) == t )
          {
            node->finalize();
          }
        }
      }
    }
  }
}

void
NodeManager::check_wfr_use()
{
  wfr_is_used_ = kernel().mpi_manager.any_true( wfr_is_used_ );

  GapJunctionEvent::set_coeff_length(
    kernel().connection_manager.get_min_delay()
    * ( kernel().simulation_manager.get_wfr_interpolation_order() + 1 ) );
}

void
NodeManager::print( std::ostream& out ) const
{
  const index max_gid = size();
  const double max_gid_width = std::floor( std::log10( max_gid ) );
  const double gid_range_width = 6 + 2 * max_gid_width;

  for ( std::vector< modelrange >::const_iterator it =
          kernel().modelrange_manager.begin();
        it != kernel().modelrange_manager.end();
        ++it )
  {
    const index first_gid = it->get_first_gid();
    const index last_gid = it->get_last_gid();
    const Model* mod = kernel().model_manager.get_model( it->get_model_id() );

    std::stringstream gid_range_strs;
    gid_range_strs << std::setw( max_gid_width + 1 ) << first_gid;
    if ( last_gid != first_gid )
    {
      gid_range_strs << " .. " << std::setw( max_gid_width + 1 ) << last_gid;
    }
    out << std::setw( gid_range_width ) << std::left << gid_range_strs.str()
        << " " << mod->get_name();

    if ( it + 1 != kernel().modelrange_manager.end() )
    {
      out << std::endl;
    }
  }
}


void
NodeManager::set_status( index gid, const DictionaryDatum& d )
{
  Node* target = local_nodes_.get_node_by_gid( gid );
  if ( target != 0 )
  {
    // node is local
    if ( target->num_thread_siblings() == 0 )
    {
      set_status_single_node_( *target, d );
    }
    else
      for ( size_t t = 0; t < target->num_thread_siblings(); ++t )
      {
        // non-root container for devices without proxies
        // we iterate over all threads
        assert( target->get_thread_sibling( t ) != 0 );
        set_status_single_node_( *( target->get_thread_sibling( t ) ), d );
      }
  }
}

void
NodeManager::get_status( DictionaryDatum& d )
{
  def< long >( d, "network_size", size() );
}

void
NodeManager::set_status( const DictionaryDatum& d )
{
}

void
NodeManager::reset_nodes_state()
{
  /* Reinitialize state on all nodes, force init_buffers() on next
     call to simulate().
     Finding all nodes is non-trivial:
     - We iterate over local nodes only.
     - Nodes without proxies are not registered in nodes_. Instead, a
       SiblingContainer is created as container, and this container is
       stored in nodes_. The container then contains the actual nodes,
       which need to be reset.
     Thus, we iterate nodes_; additionally, we iterate the content of
     a Node if it's model id is -1, which indicates that it is a
     container.  Subnets are not iterated, since their nodes are
     registered in nodes_ directly.
   */
  for ( size_t n = 0; n < local_nodes_.size(); ++n )
  {
    Node* node = local_nodes_.get_node_by_index( n );
    assert( node != 0 );
    if ( node->num_thread_siblings() == 0 ) // not a SiblingContainer
    {
      node->init_state();
      node->set_buffers_initialized( false );
    }
    else if ( node->get_model_id() == -1 )
    {
      SiblingContainer* const c = dynamic_cast< SiblingContainer* >( node );
      assert( c );
      for ( std::vector< Node* >::iterator it = c->begin(); it != c->end();
            ++it )
      {
        ( *it )->init_state();
        ( *it )->set_buffers_initialized( false );
      }
    }
  }
}
}
