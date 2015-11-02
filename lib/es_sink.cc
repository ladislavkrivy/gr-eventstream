/* -*- c++ -*- */
/*
 * Copyright 2015 Free Software Foundation, Inc.
 *
 * This file is part of gr-eventstream
 *
 * gr-eventstream is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * gr-eventstream is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with gr-eventstream; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

/*
 * config.h is generated by configure.  It contains the results
 * of probing for features, options etc.  It should be the first
 * file included in your .cc file.
 */
#include <Python.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <es/es.h>
#include <gnuradio/io_signature.h>
#include <stdio.h>

#define DEBUG(X)
//#define DEBUG(X) X

/*
 * Create a new instance of es_sink and return
 * a boost shared_ptr.  This is effectively the public constructor.
 */
es_sink_sptr
es_make_sink (
    gr_vector_int insig,
    int n_threads,
    int sample_history_in_kilosamples,
    enum es_queue_early_behaviors eb,
    enum es_search_behaviors sb,
    std::string tgroup)
{
  return es_sink_sptr (
    new es_sink (insig,n_threads,sample_history_in_kilosamples,eb,sb,tgroup));
}

/*
 * Specify constraints on number of input and output streams.
 * This info is used to construct the input and output signatures
 * (2nd & 3rd args to gr::block's constructor).  The input and
 * output signatures are used by the runtime system to
 * check that a valid number and type of inputs and outputs
 * are connected to this block.  In this case, we accept
 * only 1 input and 1 output.
 */
static const int MIN_OUT = 0;	// minimum number of output streams
static const int MAX_OUT = 0;	// maximum number of output streams

/*
 * The private constructor - NEW, with user-configurable sample history.
 */
es_sink::es_sink (
  gr_vector_int insig,
  int _n_threads,
  int _sample_history_in_kilosamples,
  enum es_queue_early_behaviors eb,
  enum es_search_behaviors sb, 
  std::string tgroup)
    : gr::sync_block (
        "es_sink",
        es_make_io_signature(insig.size(), insig),
        gr::io_signature::make (MIN_OUT, MAX_OUT, 0)),
        n_threads(_n_threads),
        d_nevents(0),
        sample_history_in_kilosamples(_sample_history_in_kilosamples),
        qq(100), dq(100), d_num_running_handlers(0),
        d_avg_ratio(tag::rolling_window::window_size=50),
        d_avg_thread_utilization(tag::rolling_window::window_size=50),
        latest_tags(pmt::make_dict()),
        d_search_behavior(sb),
        es_event_acceptor(eb,sb,tgroup)
{
    // assign live event times from our thread group
    live_event_times = d_group.live_event_times();
    live_event_times_lock = d_group.live_event_times_lock();

    d_time = 0;
    d_history = 1024*sample_history_in_kilosamples;
    set_history(d_history);

    // message port that tracks the production rate
    // for upstream schedulers
    message_port_register_out(pmt::mp("nconsumed"));
    message_port_register_out(pmt::mp("pdu_event"));

    // set up our special pdu handler
    event_queue->register_event_type("pdu_event");
    event_queue->bind_handler("pdu_event", this);
}

/*
 * Our virtual destructor.
 */
es_sink::~es_sink ()
{
    //printf("es_sink::destructor running!\n");
}

bool es_sink::start(){
    // instantiate the threadpool workers
    for(int i=0; i<n_threads; i++){
        boost::shared_ptr<es_event_loop_thread> th( new es_event_loop_thread(pmt::PMT_NIL, event_queue, &qq, &dq, &qq_cond, &d_nevents, &d_num_running_handlers) );
        threadpool.push_back( th );
    }
}

bool es_sink::stop(){
    //printf("es_sink::stop running!\n");
    wait_events();

    //printf("waiting for join\n");
    // stop all the threads in the pool
    for(int i=0; i<n_threads; i++){
        threadpool[i]->stop();
    }
    threadpool.clear();
}

void
es_sink::handler(pmt_t msg, gr_vector_void_star buf){

    pmt::pmt_t meta = pmt::tuple_ref(msg, 1);
    pmt::pmt_t vec = pmt::make_u8vector(100, 1);

    int len = event_length(msg);

    //if(buf.size() < 1 || buf.size() > 1){
    //    throw std::runtime_error("TODO: update es_sink to handle bufs != 1");
    //    }

  if(buf.size() == 1){
    switch(input_signature()->sizeof_stream_item(0)){
        case sizeof(std::complex<int16_t>):
            vec = pmt::init_s16vector(len*2, (const int16_t*) buf[0]);
            break;
        case sizeof(std::complex<float>):
            vec = pmt::init_c32vector(len, (const gr_complex*) buf[0]);
            break;
        default:
            throw std::runtime_error("TODO: update es_sink for unknown item size to pdu");
        }
        message_port_pub(pmt::mp("pdu_event"), pmt::cons(meta, vec));

    } else {
        pmt_t buflist = PMT_NIL;
        for(int i=buf.size()-1; i>=0; i--){
            switch(input_signature()->sizeof_stream_item(i)){
                case sizeof(std::complex<int16_t>):
                    vec = pmt::init_s16vector(len*2, (const int16_t*) buf[i]);
                    break;
                case sizeof(std::complex<float>):
                    vec = pmt::init_c32vector(len, (const gr_complex*) buf[i]);
                    break;
                default:
                    throw std::runtime_error("TODO: update es_sink for unknown item size to pdu");
            }
            buflist = pmt::cons(vec, buflist);
        }
        message_port_pub(pmt::mp("pdu_event"), pmt::cons(meta, buflist));
    }

    }

void
es_sink::setup_rpc()
{
#ifdef GR_CTRLPORT
    add_rpc_variable(
        rpcbasic_sptr(new rpcbasic_register_get<es_sink, int>(
            alias(), "nevents ready_running",
            &es_sink::num_events,
            pmt::mp(0.0f), pmt::mp(0.0f), pmt::mp(0.0f),
            "count", "Num events ready/running.", RPC_PRIVLVL_MIN,
            DISPTIME | DISPOPTSTRIP)
        )
    );

    add_rpc_variable(
        rpcbasic_sptr(new rpcbasic_register_get<es_sink, uint64_t>(
            alias(), "nevents discarded",
            &es_sink::num_discarded,
            pmt::mp(0.0f), pmt::mp(0.0f), pmt::mp(0.0f),
            "count", "Num events discarded (event time < min buffer time).", RPC_PRIVLVL_MIN,
            DISPTIME | DISPOPTSTRIP)
        )
    );

    add_rpc_variable(
        rpcbasic_sptr(new rpcbasic_register_get<es_sink, uint64_t>(
            alias(), "nevents ASAP",
            &es_sink::num_asap,
            pmt::mp(0.0f), pmt::mp(0.0f), pmt::mp(0.0f),
            "count", "Num events handled ASAP (event time < min buffer time).", RPC_PRIVLVL_MIN,
            DISPTIME | DISPOPTSTRIP)
        )
    );

    add_rpc_variable(
        rpcbasic_sptr(new rpcbasic_register_get<es_sink, uint64_t>(
            alias(), "nevents soon",
            &es_sink::num_soon,
            pmt::mp(0.0f), pmt::mp(0.0f), pmt::mp(0.0f),
            "count", "Num events received too soon (event time + duration > max buffer time).", RPC_PRIVLVL_MIN,
            DISPTIME | DISPOPTSTRIP)
        )
    );

    add_rpc_variable(
        rpcbasic_sptr(new rpcbasic_register_get<es_sink, uint64_t>(
            alias(), "nevents added",
            &es_sink::num_events_added,
            pmt::mp(0.0f), pmt::mp(0.0f), pmt::mp(0.0f),
            "count", "Num events added to event_queue.", RPC_PRIVLVL_MIN,
            DISPTIME | DISPOPTSTRIP)
        )
    );

    add_rpc_variable(
        rpcbasic_sptr(new rpcbasic_register_get<es_sink, uint64_t>(
            alias(), "nevents removed",
            &es_sink::num_events_removed,
            pmt::mp(0.0f), pmt::mp(0.0f), pmt::mp(0.0f),
            "count", "Num events removed from event_queue.", RPC_PRIVLVL_MIN,
            DISPTIME | DISPOPTSTRIP)
        )
    );

    add_rpc_variable(
        rpcbasic_sptr(new rpcbasic_register_get<es_sink, uint64_t>(
            alias(), "time in buff window",
            &es_sink::buffer_window_size,
            pmt::mp(0.0f), pmt::mp(0.0f), pmt::mp(0.0f),
            "num samples", "Size of history buffer.", RPC_PRIVLVL_MIN,
            DISPTIME | DISPOPTSTRIP)
        )
    );

    add_rpc_variable(
        rpcbasic_sptr(new rpcbasic_register_get<es_sink, uint64_t>(
            alias(), "time of curr event",
            &es_sink::event_time,
            pmt::mp(0.0f), pmt::mp(0.0f), pmt::mp(0.0f),
            "sample num", "Current event time.", RPC_PRIVLVL_MIN,
            DISPTIME | DISPOPTSTRIP)
        )
    );

    add_rpc_variable(
        rpcbasic_sptr(new rpcbasic_register_get<es_sink, uint64_t>(
            alias(), "nevent hndls run",
            &es_sink::num_running_handlers,
            pmt::mp(0.0f), pmt::mp(0.0f), pmt::mp(0.0f),
            "count", "Num event handlers running.", RPC_PRIVLVL_MIN,
            DISPTIME | DISPOPTSTRIP)
        )
    );

    add_rpc_variable(
        rpcbasic_sptr(new rpcbasic_register_get<es_sink, uint64_t>(
            alias(), "nevents event_queue",
            &es_sink::event_queue_size,
            pmt::mp(0.0f), pmt::mp(0.0f), pmt::mp(0.0f),
            "count", "Size of event_queue (num events not yet ready/running).", RPC_PRIVLVL_MIN,
            DISPTIME | DISPOPTSTRIP)
        )
    );

    add_rpc_variable(
        rpcbasic_sptr(new rpcbasic_register_get<es_sink, double>(
            alias(), "eventAvgRunRatio",
            &es_sink::event_run_ratio,
            pmt::mp(0.0f), pmt::mp(0.0f), pmt::mp(0.0f),
            "%", "Avg Ratio of running events to total ready/running events.", RPC_PRIVLVL_MIN,
            DISPTIME | DISPOPTSTRIP)
        )
    );

    add_rpc_variable(
        rpcbasic_sptr(new rpcbasic_register_get<es_sink, double>(
            alias(), "eventAvgThreadUtilization",
            &es_sink::event_thread_utilization,
            pmt::mp(0.0f), pmt::mp(0.0f), pmt::mp(0.0f),
            "%", "Avg Ratio of running threads to total threads.", RPC_PRIVLVL_MIN,
            DISPTIME | DISPOPTSTRIP)
        )
    );
#endif
}

int
es_sink::num_events()
{
    return (int)d_nevents;
}

uint64_t
es_sink::num_discarded()
{
    return event_queue->d_num_discarded;
}

uint64_t
es_sink::num_asap()
{
    return event_queue->d_num_asap;
}

uint64_t
es_sink::num_events_added()
{
    return event_queue->d_num_events_added;
}

uint64_t
es_sink::num_events_removed()
{
    return event_queue->d_num_events_removed;
}

uint64_t
es_sink::event_time()
{
    return event_queue->d_event_time;
}

uint64_t
es_sink::buffer_window_size()
{
    return d_buffer_window_size;
}

uint64_t
es_sink::num_soon()
{
    return event_queue->d_num_soon;
}

uint64_t
es_sink::num_running_handlers()
{
    return (uint64_t)d_num_running_handlers;
}

uint64_t
es_sink::event_queue_size()
{
    return (uint64_t)event_queue->length();
}

double
es_sink::event_run_ratio()
{
    double ret = 0.0;

    if (d_nevents > 0)
    {
        ret = (double)((double) d_num_running_handlers / d_nevents) * 100.0;
    }
    d_avg_ratio(ret);
    return rolling_mean(d_avg_ratio);
}

double
es_sink::event_thread_utilization()
{
    double ret = 0.0;

    if (n_threads > 0)
    {
        ret = (double)((double) d_num_running_handlers / n_threads) * 100.0;
    }
    d_avg_thread_utilization(ret);
    return rolling_mean(d_avg_thread_utilization);
}

/**
 * @brief Search forward through live_event_times to find an insertion index.
 *
 * Search forward starting at the beginning of the live_event_times list and
 * continuing until either an appropriate insertion index is found or the end
 * of the list is reached.
 *
 * @param [in] evt_time Event time to insert into the live_event_times list.
 *
 * @return Index at which evt_time should be inserted to maintain sort.
 */
size_t
es_sink::find_forward(const uint64_t& evt_time)
{
  size_t idx = 0, sz = live_event_times->size();
  for (idx = 0; idx < sz && evt_time > live_event_times->at(idx); idx++){}
  return idx;
}

/**
 * @brief Search backward through live_event_times to find an insertion index.
 *
 * Search backward starting at the end of the live_event_times list and
 * continuing until either an appropriate insertion index is found or the
 * beginning of the list is reached.
 *
 * @param [in] evt_time Event time to insert into the live_event_times list.
 *
 * @return Index at which evt_time should be inserted to maintain sort.
 */
size_t
es_sink::find_reverse(const uint64_t& evt_time)
{
  size_t sz = live_event_times->size(), idx = 0;

  // If nothing is in the vector then the insertion index must be 0.
  if (sz == 0)
  {
    live_event_times_lock->unlock();
    return 0;
  }

  for (idx = sz; idx-- > 0 && evt_time < live_event_times->at(idx);){}

  return idx + 1;
}

/**
 * @brief Comparison function used by the binary search method find_binary().
 *
 * @param [in] vval Reference to an item in the live_event_times vector (vector
 *   value).
 * @param [in] cval Reference to an item to be inserted into the
 *   live_event_times vector (comparison value).
 */
bool sink_compare(const uint64_t& vval, const uint64_t& cval)
{
  return cval > vval;
};

/**
 * @brief Search through a sorted list using a binary pattern to find an
 *   insertion index.
 *
 * Search using a binary pattern starting at the beginning of the
 * live_event_times list and continuing until either an appropriate insertion
 * index is found or the binary search is exhausted.
 *
 * @param [in] evt_time Event time to insert into the live_event_times list.
 *
 * @return Index at which evt_time should be inserted to maintain sort.
 */
size_t
es_sink::find_binary(const uint64_t& evt_time)
{
    
    return std::lower_bound(
      live_event_times->begin(),
      live_event_times->end(),
      evt_time,
      sink_compare) - live_event_times->begin();
}

/**
 * @brief Search using the preconfigured search type for an insertion index.
 *
 * This is a wrapper method to call the appropriate search method based on the
 * value of the d_search_behavior member variable.
 *
 * @param [in] evt_time Event time to insert into the live_event_times list.
 *
 * @return Index at which evt_time should be inserted to maintain sort.
 */
int es_sink::find_index(const uint64_t& evt_time)
{
    switch(d_search_behavior)
    {
        case SEARCH_BINARY:
            return find_binary(evt_time);
        case SEARCH_REVERSE:
            return find_reverse(evt_time);
        case SEARCH_FORWARD:
            return find_forward(evt_time);
        default:
            return find_forward(evt_time);
    }
}

int
es_sink::work (int noutput_items,
			gr_vector_const_void_star &input_items,
			gr_vector_void_star &output_items)
{
  // keep up with the latest tags
  std::vector <gr::tag_t> v;
  get_tags_in_range(v,0,nitems_read(0),nitems_read(0)+noutput_items);
  for(int i=0; i<v.size(); i++){
    latest_tags = pmt::dict_add(latest_tags, v[i].key, pmt::cons(pmt::from_uint64(v[i].offset), v[i].value));
    }

  char *in = (char*) input_items[0];

  //printf("entered es_sink::work()\n");
  // compute the min and max sample times currently accessible in the buffer
  unsigned long long max_time = d_time + noutput_items;
  unsigned long long min_time = (d_history > d_time)?0:d_time-d_history+1;

  d_buffer_window_size = max_time - min_time;

  // generate an empty event sptr
  es_eh_pair *eh = NULL;

  unsigned long long delete_index = 0;

  live_event_times_lock->lock(); //TODO: can we move this inside the loop for better performance?
  while( dq.pop(delete_index) ){
	// remove the event time from the event live times l
    for(int i=0; i<live_event_times->size(); i++){
        if(live_event_times->at(i) == delete_index){
            live_event_times->erase(live_event_times->begin()+i);
            break;
            }
        }
    }
  live_event_times_lock->unlock();


  // while we can service events with the current buffer, get them and handle them.
  while( this->locked_fetch_next_event( min_time, max_time, &eh ) ){

   DEBUG( printf("es::sink work() got event\n"); )
  //  int a = d_nevents;
 //   printf("incrementing d_nevents (%d->%d)\n", a, a+1);
    d_nevents++;

//    printf("es_sink::work()::fetched event successfully (%llu --> %llu)\n",min_time,max_time);
    pmt_t event = eh->event;
    uint64_t etime = ::event_time(eh->event);

    // compute the local buffer offset of the event
    int buffer_offset = (int)(etime - d_time + d_history - 1);

    //printf("event(%lu), time(%lu), history(%lu), offset(%d)\n",
    //        etime, d_time, d_history, buffer_offset);
    if(buffer_offset < 0) {
        printf("WARNING: bad buffer_offset: %d, Dropping Data!\n",buffer_offset);
        buffer_offset = 0;
    }

    pmt_t buf_list = PMT_NIL;
    bool first_item = true;

    // loop over each input buffer copying contents into pmt_buffers to tag onto event
    for(int i=0; i<input_items.size(); i++){

        // alocate a new pmt u8 vector to store buffer contents in.
        pmt_t buf_i = pmt::init_u8vector( d_input_signature->sizeof_stream_item(i)*eh->length(), (const uint8_t*) input_items[i] + (buffer_offset * d_input_signature->sizeof_stream_item(i)) );

        // build up a pmt list containing pmt_u8vectors with all the buffers
        buf_list = pmt::list_add(buf_list, buf_i);
    }

    // register the buffer in the event
    DEBUG(printf("reg buffer: ");)
    DEBUG(pmt::print(buf_list);)
    DEBUG(printf("\n");)
    event = pmt::make_tuple( es::type_es_event, pmt::dict_update( pmt::tuple_ref( event, 1), latest_tags ) );
    event = register_buffer( event, buf_list );
    eh->event = event;

    // insert event time into thread notification list and notify
    qq.push(eh);
    qq_cond.notify_one();

  }

  // consume the current input items
  live_event_times_lock->lock();
  int nconsume = (int)std::min(
                    (uint64_t)noutput_items,
                    std::min(
                        live_event_times->size()==0?
                            noutput_items :
                            (uint64_t)(live_event_times->at(0) - min_time),
                        event_queue->empty()?
                            noutput_items :
                            (uint64_t)(event_queue->min_time() - min_time)
                        )
                    );
  live_event_times_lock->unlock();

  // make sure worker threads are working on live events
  if(nconsume != noutput_items)
    qq_cond.notify_one();

  // if we can not consume any more while waiting for the next event - yield so handler can finish
  if(nconsume == 0)
	boost::this_thread::yield();
//    boost::this_thread::sleep_for(boost::chrono::milliseconds(1));

  d_time += nconsume;
  message_port_pub(pmt::mp("nconsumed"), pmt::mp(d_time));
  return nconsume;
}

void es_sink::wait_events(){
    // wait for all events to get picked up by threads
    while(d_nevents>0){
        // we need to allow our python flowgraph handlers to be able to grab the GIL here...
        qq_cond.notify_all();
        //Py_BEGIN_ALLOW_THREADS
        boost::this_thread::yield();
        //Py_END_ALLOW_THREADS
        }
}

int es_sink::locked_fetch_next_event(unsigned long long min, unsigned long long max, es_eh_pair **eh){
    live_event_times_lock->lock();
    if(event_queue->fetch_next_event( min, max, eh ) == false){
        live_event_times_lock->unlock();
        return false;
        }
 
    // add it to live events list before releasing lock
    uint64_t etime = ::event_time((*eh)->event);
    int live_event_times_insert_offset = find_index(etime);
    live_event_times->insert(live_event_times->begin() + live_event_times_insert_offset, etime);
    live_event_times_lock->unlock();
    return true;
}

