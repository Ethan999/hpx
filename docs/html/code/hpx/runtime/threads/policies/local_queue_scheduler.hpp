//  Copyright (c) 2007-2013 Hartmut Kaiser
//  Copyright (c) 2011      Bryce Lelbach
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#if !defined(HPX_THREADMANAGER_SCHEDULING_LOCAL_QUEUE_MAR_15_2011_0926AM)
#define HPX_THREADMANAGER_SCHEDULING_LOCAL_QUEUE_MAR_15_2011_0926AM

#include <vector>
#include <memory>

#include <hpx/config.hpp>
#include <hpx/exception.hpp>
#include <hpx/util/logging.hpp>
#include <hpx/runtime/threads/thread_data.hpp>
#include <hpx/runtime/threads/topology.hpp>
#include <hpx/runtime/threads/policies/thread_queue.hpp>
#include <hpx/runtime/threads/policies/affinity_data.hpp>
#include <hpx/runtime/threads/policies/scheduler_base.hpp>

#include <boost/noncopyable.hpp>
#include <boost/atomic.hpp>
#include <boost/mpl/bool.hpp>

#include <hpx/config/warnings_prefix.hpp>

// TODO: add branch prediction and function heat

///////////////////////////////////////////////////////////////////////////////
namespace hpx { namespace threads { namespace policies
{
#if HPX_THREAD_MINIMAL_DEADLOCK_DETECTION
    ///////////////////////////////////////////////////////////////////////////
    // We globally control whether to do minimal deadlock detection using this
    // global bool variable. It will be set once by the runtime configuration
    // startup code
    extern bool minimal_deadlock_detection;
#endif

    ///////////////////////////////////////////////////////////////////////////
    /// The local_queue_scheduler maintains exactly one queue of work
    /// items (threads) per OS thread, where this OS thread pulls its next work
    /// from. 
    template <typename Mutex
            , typename PendingQueuing
            , typename StagedQueuing
            , typename TerminatedQueuing
             >
    class local_queue_scheduler : public scheduler_base
    {
    protected:
        // The maximum number of active threads this thread manager should
        // create. This number will be a constraint only as long as the work
        // items queue is not empty. Otherwise the number of active threads
        // will be incremented in steps equal to the \a min_add_new_count
        // specified above.
        // FIXME: this is specified both here, and in thread_queue.
        enum { max_thread_count = 1000 };

    public:
        typedef boost::mpl::false_ has_periodic_maintenance;

        typedef thread_queue<
            Mutex, PendingQueuing, StagedQueuing, TerminatedQueuing
        > thread_queue_type;

        // the scheduler type takes two initialization parameters:
        //    the number of queues
        //    the number of high priority queues
        //    the maxcount per queue
        struct init_parameter
        {
            init_parameter()
              : num_queues_(1),
                max_queue_thread_count_(max_thread_count),
                numa_sensitive_(false)
            {}

            init_parameter(std::size_t num_queues,
                    std::size_t max_queue_thread_count = max_thread_count,
                    bool numa_sensitive = false)
              : num_queues_(num_queues),
                max_queue_thread_count_(max_queue_thread_count),
                numa_sensitive_(numa_sensitive)
            {}

            std::size_t num_queues_;
            std::size_t max_queue_thread_count_;
            bool numa_sensitive_;
        };
        typedef init_parameter init_parameter_type;

        local_queue_scheduler(init_parameter_type const& init)
          : scheduler_base(init.num_queues_),
            max_queue_thread_count_(init.max_queue_thread_count_), 
            queues_(init.num_queues_),
            curr_queue_(0),
            numa_sensitive_(init.numa_sensitive_),
#if !defined(HPX_NATIVE_MIC)        // we know that the MIC has one NUMA domain only
            steals_in_numa_domain_(init.num_queues_),
            steals_outside_numa_domain_(init.num_queues_),
#endif
#if !defined(HPX_HAVE_MORE_THAN_64_THREADS) || defined(HPX_MAX_CPU_COUNT)
            numa_domain_masks_(init.num_queues_),
            outside_numa_domain_masks_(init.num_queues_)
#else
            numa_domain_masks_(init.num_queues_, topology_.get_machine_affinity_mask()),
            outside_numa_domain_masks_(init.num_queues_, topology_.get_machine_affinity_mask())
#endif
        {
        }

        virtual ~local_queue_scheduler()
        {
            for (std::size_t i = 0; i < queues_.size(); ++i)
                delete queues_[i];
        }

        bool numa_sensitive() const { return numa_sensitive_; }

#if HPX_THREAD_MAINTAIN_CREATION_AND_CLEANUP_RATES
        boost::uint64_t get_creation_time(bool reset)
        {
            boost::uint64_t time = 0;

            for (std::size_t i = 0; i < queues_.size(); ++i)
                time += queues_[i]->get_creation_time(reset);

            return time;
        }

        boost::uint64_t get_cleanup_time(bool reset)
        {
            boost::uint64_t time = 0;

            for (std::size_t i = 0; i < queues_.size(); ++i)
                time += queues_[i]->get_cleanup_time(reset);

            return time;
        }
#endif

        std::size_t get_num_pending_misses(std::size_t num_thread, bool reset)
        {
            std::size_t num_pending_misses = 0;
            if (num_thread == std::size_t(-1))
            {
                for (std::size_t i = 0; i < queues_.size(); ++i)
                    num_pending_misses += queues_[i]->
                        get_num_pending_misses(reset);

                return num_pending_misses;
            }

            num_pending_misses += queues_[num_thread]->
                get_num_pending_misses(reset);
            return num_pending_misses;
        }

        std::size_t get_num_pending_accesses(std::size_t num_thread, bool reset)
        {
            std::size_t num_pending_accesses = 0;
            if (num_thread == std::size_t(-1))
            {
                for (std::size_t i = 0; i < queues_.size(); ++i)
                    num_pending_accesses += queues_[i]->
                        get_num_pending_accesses(reset);

                return num_pending_accesses;
            }

            num_pending_accesses += queues_[num_thread]->
                get_num_pending_accesses(reset);
            return num_pending_accesses;
        }

        std::size_t get_num_stolen_from_pending(std::size_t num_thread, bool reset)
        {
            std::size_t num_stolen_threads = 0;
            if (num_thread == std::size_t(-1))
            {
                for (std::size_t i = 0; i < queues_.size(); ++i)
                    num_stolen_threads += queues_[i]->get_num_stolen_from_pending(reset);
                return num_stolen_threads;
            }

            num_stolen_threads += queues_[num_thread]->get_num_stolen_from_pending(reset);
            return num_stolen_threads;
        }

        std::size_t get_num_stolen_to_pending(std::size_t num_thread, bool reset)
        {
            std::size_t num_stolen_threads = 0;
            if (num_thread == std::size_t(-1))
            {
                for (std::size_t i = 0; i < queues_.size(); ++i)
                    num_stolen_threads += queues_[i]->get_num_stolen_to_pending(reset);
                return num_stolen_threads;
            }

            num_stolen_threads += queues_[num_thread]->get_num_stolen_to_pending(reset);
            return num_stolen_threads;
        }

        std::size_t get_num_stolen_from_staged(std::size_t num_thread, bool reset)
        {
            std::size_t num_stolen_threads = 0;
            if (num_thread == std::size_t(-1))
            {
                for (std::size_t i = 0; i < queues_.size(); ++i)
                    num_stolen_threads += queues_[i]->get_num_stolen_from_staged(reset);
                return num_stolen_threads;
            }

            num_stolen_threads += queues_[num_thread]->get_num_stolen_from_staged(reset);
            return num_stolen_threads;
        }

        std::size_t get_num_stolen_to_staged(std::size_t num_thread, bool reset)
        {
            std::size_t num_stolen_threads = 0;
            if (num_thread == std::size_t(-1))
            {
                for (std::size_t i = 0; i < queues_.size(); ++i)
                    num_stolen_threads += queues_[i]->get_num_stolen_to_staged(reset);
                return num_stolen_threads;
            }

            num_stolen_threads += queues_[num_thread]->get_num_stolen_to_staged(reset);
            return num_stolen_threads;
        }

        ///////////////////////////////////////////////////////////////////////
        void abort_all_suspended_threads()
        {
            for (std::size_t i = 0; i < queues_.size(); ++i)
                queues_[i]->abort_all_suspended_threads(i);
        }

        ///////////////////////////////////////////////////////////////////////
        bool cleanup_terminated(bool delete_all = false)
        {
            bool empty = true;
            for (std::size_t i = 0; i < queues_.size(); ++i)
                empty = queues_[i]->cleanup_terminated(delete_all) && empty;
            return empty;
        }

        ///////////////////////////////////////////////////////////////////////
        // create a new thread and schedule it if the initial state is equal to
        // pending
        thread_id_type create_thread(thread_init_data& data,
            thread_state_enum initial_state, bool run_now, error_code& ec,
            std::size_t num_thread)
        {
#if HPX_THREAD_MAINTAIN_TARGET_ADDRESS
            // try to figure out the NUMA node where the data lives
            if (numa_sensitive_ && std::size_t(-1) == num_thread) {
                mask_cref_type mask =
                    topology_.get_thread_affinity_mask_from_lva(data.lva);
                if (any(mask)) {
                    num_thread = find_first(mask);
                }
            }
#endif
            std::size_t queue_size = queues_.size();

            if (std::size_t(-1) == num_thread)
                num_thread = ++curr_queue_ % queue_size;

            if (num_thread >= queue_size)
                num_thread %= queue_size;

            HPX_ASSERT(num_thread < queue_size);
            return queues_[num_thread]->create_thread(data, initial_state,
                run_now, num_thread, ec);
        }

        /// Return the next thread to be executed, return false if none is
        /// available
        virtual bool get_next_thread(std::size_t num_thread, bool running,
            boost::int64_t& idle_loop_count, threads::thread_data_base*& thrd)
        {
            std::size_t queues_size = queues_.size();

            {
                HPX_ASSERT(num_thread < queues_size);
                bool result = queues_[num_thread]->get_next_thread(thrd, num_thread);

                queues_[num_thread]->increment_num_pending_accesses();

                if (!result)
                {
                    queues_[num_thread]->increment_num_pending_misses();

                    bool have_staged = (queues_[num_thread]->
                        get_staged_queue_length(boost::memory_order_relaxed) != 0);

                    // Give up, we should have work to convert.
                    if (have_staged)
                        return false;
                }
                else
                    return true;
            }

            if (numa_sensitive_)
            {
                mask_cref_type this_numa_domain = numa_domain_masks_[num_thread];
                mask_cref_type numa_domain = outside_numa_domain_masks_[num_thread];
    
                // steal thread from other queue
                for (std::size_t i = 1; i < queues_size; ++i)
                {
                    // FIXME: Do a better job here.
                    std::size_t const idx = (i + num_thread) % queues_size;

                    HPX_ASSERT(idx != num_thread);

                    if (!test(this_numa_domain, idx) && !test(numa_domain, idx))
                        continue;
    
                    if (queues_[idx]->get_next_thread(thrd, num_thread))
                    {
                        queues_[idx]->increment_num_stolen_from_pending();
                        queues_[num_thread]->increment_num_stolen_to_pending();
                        return true;
                    }
                }
            }

            else // not NUMA-sensitive
            {
                for (std::size_t i = 1; i < queues_size; ++i)
                {
                    // FIXME: Do a better job here.
                    std::size_t const idx = (i + num_thread) % queues_size;

                    HPX_ASSERT(idx != num_thread);

                    if (queues_[idx]->get_next_thread(thrd, num_thread))
                    {
                        queues_[idx]->increment_num_stolen_from_pending();
                        queues_[num_thread]->increment_num_stolen_to_pending();
                        return true;
                    }
                }
            }

            return false;
        }

        /// Schedule the passed thread
        void schedule_thread(threads::thread_data_base* thrd, std::size_t num_thread,
            thread_priority priority = thread_priority_normal)
        {
            if (std::size_t(-1) == num_thread)
                num_thread = ++curr_queue_ % queues_.size();

            HPX_ASSERT(num_thread < queues_.size());
            queues_[num_thread]->schedule_thread(thrd, num_thread);
        }

        void schedule_thread_last(threads::thread_data_base* thrd, std::size_t num_thread,
            thread_priority priority = thread_priority_normal)
        {
            local_queue_scheduler::schedule_thread(thrd, num_thread, priority);
        }

        /// Destroy the passed thread as it has been terminated
        bool destroy_thread(threads::thread_data_base* thrd, boost::int64_t& busy_count)
        {
            for (std::size_t i = 0; i < queues_.size(); ++i)
            {
                if (queues_[i]->destroy_thread(thrd, busy_count))
                    return true;
            }

            // the thread has to belong to one of the queues, always
            HPX_ASSERT(false);

            return false;
        }

        ///////////////////////////////////////////////////////////////////////
        // This returns the current length of the queues (work items and new items)
        boost::int64_t get_queue_length(std::size_t num_thread = std::size_t(-1)) const
        {
            // Return queue length of one specific queue.
            boost::int64_t count = 0;
            if (std::size_t(-1) != num_thread) {
                HPX_ASSERT(num_thread < queues_.size());

                return queues_[num_thread]->get_queue_length();
            }

            for (std::size_t i = 0; i < queues_.size(); ++i)
                count += queues_[i]->get_queue_length();

            return count;
        }

        ///////////////////////////////////////////////////////////////////////
        // Queries the current thread count of the queues.
        boost::int64_t get_thread_count(thread_state_enum state = unknown,
            thread_priority priority = thread_priority_default,
            std::size_t num_thread = std::size_t(-1), bool reset = false) const
        {
            // Return thread count of one specific queue.
            boost::int64_t count = 0;
            if (std::size_t(-1) != num_thread)
            {
                HPX_ASSERT(num_thread < queues_.size());

                switch (priority) {
                case thread_priority_default:
                case thread_priority_low:
                case thread_priority_normal:
                case thread_priority_critical:
                    return queues_[num_thread]->get_thread_count(state);

                default:
                case thread_priority_unknown:
                    {
                        HPX_THROW_EXCEPTION(bad_parameter,
                            "local_queue_scheduler::get_thread_count",
                            "unknown thread priority value (thread_priority_unknown)");
                        return 0;
                    }
                }
                return 0;
            }

            // Return the cumulative count for all queues.
            switch (priority) {
            case thread_priority_default:
            case thread_priority_low:
            case thread_priority_normal:
            case thread_priority_critical:
                {
                    for (std::size_t i = 0; i < queues_.size(); ++i)
                        count += queues_[i]->get_thread_count(state);
                    break;
                }

            default:
            case thread_priority_unknown:
                {
                    HPX_THROW_EXCEPTION(bad_parameter,
                        "local_queue_scheduler::get_thread_count",
                        "unknown thread priority value (thread_priority_unknown)");
                    return 0;
                }
            }
            return count;
        }

#if HPX_THREAD_MAINTAIN_QUEUE_WAITTIME
        ///////////////////////////////////////////////////////////////////////
        // Queries the current average thread wait time of the queues.
        boost::int64_t get_average_thread_wait_time(
            std::size_t num_thread = std::size_t(-1)) const
        {
            // Return average thread wait time of one specific queue.
            boost::uint64_t wait_time = 0;
            boost::uint64_t count = 0;
            if (std::size_t(-1) != num_thread)
            {
                HPX_ASSERT(num_thread < queues_.size());

                wait_time += queues_[num_thread]->get_average_thread_wait_time();
                return wait_time / (count + 1);
            }

            for (std::size_t i = 0; i < queues_.size(); ++i)
            {
                wait_time += queues_[i]->get_average_thread_wait_time();
                ++count;
            }

            return wait_time / (count + 1);
        }

        ///////////////////////////////////////////////////////////////////////
        // Queries the current average task wait time of the queues.
        boost::int64_t get_average_task_wait_time(
            std::size_t num_thread = std::size_t(-1)) const
        {
            // Return average task wait time of one specific queue.
            boost::uint64_t wait_time = 0;
            boost::uint64_t count = 0;
            if (std::size_t(-1) != num_thread)
            {
                HPX_ASSERT(num_thread < queues_.size());

                wait_time += queues_[num_thread]->get_average_task_wait_time();
                return wait_time / (count + 1);
            }

            for (std::size_t i = 0; i < queues_.size(); ++i)
            {
                wait_time += queues_[i]->get_average_task_wait_time();
                ++count;
            }

            return wait_time / (count + 1);
        }
#endif

        /// This is a function which gets called periodically by the thread
        /// manager to allow for maintenance tasks to be executed in the
        /// scheduler. Returns true if the OS thread calling this function
        /// has to be terminated (i.e. no more work has to be done).
        virtual bool wait_or_add_new(std::size_t num_thread, bool running,
            boost::int64_t& idle_loop_count)
        {
            std::size_t queues_size = queues_.size();
            HPX_ASSERT(num_thread < queues_.size());

            std::size_t added = 0;
            bool result = true;

            result = queues_[num_thread]->wait_or_add_new(
                num_thread, running, idle_loop_count, added) && result;
            if (0 != added) return result;

            if (numa_sensitive_)
            {
                // steal work items: first try to steal from other cores in
                // the same NUMA node
#if !defined(HPX_NATIVE_MIC)        // we know that the MIC has one NUMA domain only
                if (test(steals_in_numa_domain_, num_thread))
#endif
                {
                    mask_cref_type numa_domain_mask =
                        numa_domain_masks_[num_thread];
                    for (std::size_t i = 1; i < queues_size; ++i)
                    {
                        // FIXME: Do a better job here.
                        std::size_t const idx = (i + num_thread) % queues_size;

                        HPX_ASSERT(idx != num_thread);

                        if (!test(numa_domain_mask, topology_.get_pu_number(idx)))
                            continue;

                        result = queues_[num_thread]->wait_or_add_new(num_thread,
                            running, idle_loop_count, added, queues_[idx]) && result;
                        if (0 != added)
                        {
                            queues_[idx]->increment_num_stolen_from_staged(added);
                            queues_[num_thread]->increment_num_stolen_to_staged(added);
                            return result;
                        }
                    }
                }

#if !defined(HPX_NATIVE_MIC)        // we know that the MIC has one NUMA domain only
                // if nothing found, ask everybody else
                if (test(steals_outside_numa_domain_, num_thread)) {
                    mask_cref_type numa_domain_mask =
                        outside_numa_domain_masks_[num_thread];
                    for (std::size_t i = 1; i < queues_size; ++i)
                    {
                        // FIXME: Do a better job here.
                        std::size_t const idx = (i + num_thread) % queues_size;

                        HPX_ASSERT(idx != num_thread);

                        if (!test(numa_domain_mask, topology_.get_pu_number(idx)))
                            continue;

                        result = queues_[num_thread]->wait_or_add_new(num_thread,
                            running, idle_loop_count, added, queues_[idx]) && result;
                        if (0 != added)
                        {
                            queues_[idx]->increment_num_stolen_from_staged(added);
                            queues_[num_thread]->increment_num_stolen_to_staged(added);
                            return result;
                        }
                    }
                }
#endif
            }

            else // not NUMA-sensitive
            {
                for (std::size_t i = 1; i < queues_size; ++i)
                {
                    // FIXME: Do a better job here.
                    std::size_t const idx = (i + num_thread) % queues_size;

                    HPX_ASSERT(idx != num_thread);

                    result = queues_[num_thread]->wait_or_add_new(num_thread,
                        running, idle_loop_count, added, queues_[idx]) && result;
                    if (0 != added)
                    {
                        queues_[idx]->increment_num_stolen_from_staged(added);
                        queues_[num_thread]->increment_num_stolen_to_staged(added);
                        return result;
                    }
                }
            }

#if HPX_THREAD_MINIMAL_DEADLOCK_DETECTION
            // no new work is available, are we deadlocked?
            if (HPX_UNLIKELY(minimal_deadlock_detection && LHPX_ENABLED(error)))
            {
                bool suspended_only = true;

                for (std::size_t i = 0; suspended_only && i != queues_.size(); ++i) {
                    suspended_only = queues_[i]->dump_suspended_threads(
                        i, idle_loop_count, running);
                }

                if (HPX_UNLIKELY(suspended_only)) {
                    if (running) {
                        LTM_(error) //-V128
                            << "queue(" << num_thread << "): "
                            << "no new work available, are we deadlocked?";
                    }
                    else {
                        LHPX_CONSOLE_(hpx::util::logging::level::error) << "  [TM] " //-V128
                              << "queue(" << num_thread << "): "
                              << "no new work available, are we deadlocked?\n";
                    }
                }
            }
#endif

            return result;
        }

        ///////////////////////////////////////////////////////////////////////
        void on_start_thread(std::size_t num_thread)
        {
            queues_[num_thread] =
                new thread_queue_type(max_queue_thread_count_);

            queues_[num_thread]->on_start_thread(num_thread);

            // pre-calculate certain constants for the given thread number
            std::size_t num_pu = get_pu_num(num_thread);
            mask_cref_type machine_mask = topology_.get_machine_affinity_mask();
            mask_cref_type core_mask =
                topology_.get_thread_affinity_mask(num_pu, numa_sensitive_);
            mask_cref_type node_mask =
                topology_.get_numa_node_affinity_mask(num_pu, numa_sensitive_);

            if (any(core_mask) && any(node_mask)) {
#if !defined(HPX_NATIVE_MIC)        // we know that the MIC has one NUMA domain only
                set(steals_in_numa_domain_, num_thread);
#endif
                numa_domain_masks_[num_thread] = node_mask;
            }

            // we allow the thread on the boundary of the NUMA domain to steal
            mask_type first_mask = mask_type();
            resize(first_mask, mask_size(core_mask));

            std::size_t first = find_first(node_mask);
            if (first != std::size_t(-1))
                set(first_mask, first);
            else
                first_mask = core_mask;

            if (any(first_mask & core_mask)) {
#if !defined(HPX_NATIVE_MIC)        // we know that the MIC has one NUMA domain only
                set(steals_outside_numa_domain_, num_thread);
#endif
                outside_numa_domain_masks_[num_thread] = not_(node_mask) & machine_mask;
            }
        }

        void on_stop_thread(std::size_t num_thread)
        {
            queues_[num_thread]->on_stop_thread(num_thread);
        }

        void on_error(std::size_t num_thread, boost::exception_ptr const& e)
        {
            queues_[num_thread]->on_error(num_thread, e);
        }

    protected:
        std::size_t max_queue_thread_count_;
        std::vector<thread_queue_type*> queues_;   
        boost::atomic<std::size_t> curr_queue_;
        bool numa_sensitive_;

#if !defined(HPX_NATIVE_MIC)        // we know that the MIC has one NUMA domain only
        mask_type steals_in_numa_domain_;
        mask_type steals_outside_numa_domain_;
#endif
        std::vector<mask_type> numa_domain_masks_;
        std::vector<mask_type> outside_numa_domain_masks_;
    };
}}}

#include <hpx/config/warnings_suffix.hpp>

#endif
