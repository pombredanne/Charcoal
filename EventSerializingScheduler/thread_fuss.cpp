/* Copyright Benjamin Ylvisaker */
/*
 *  This file contains an ISA-portable PIN tool for tracing system calls
 */

#include <stdlib.h>
#include <stdio.h>
#include <sys/time.h>

#if defined(TARGET_MAC)
#include <sys/syscall.h>
#elif !defined(TARGET_WINDOWS)
#include <syscall.h>
#endif

#include "pin.H"
#include "assert_efc.h"

#define safe_dec( e ) \
    ({ typeof( e ) *xp = &e; \
       typeof( e ) XSAFE_DEC = *xp; \
       --(*xp); \
       assert_RFL( *xp < XSAFE_DEC ); \
       *xp; })

#define safe_inc( e ) \
    ({ typeof( e ) *xp = &e; \
       typeof( e ) XSAFE_INC = *xp; \
       ++(*xp); \
       assert_RFL( *xp > XSAFE_INC ); \
       *xp; })

#define LOGF(...) do{ fprintf( log_file, __VA_ARGS__ ); fflush( log_file ); } while(0)

static bool tv_greater_than( struct timeval *t1, struct timeval *t2 )
{
    if( t1->tv_sec > t2->tv_sec )
        return true;
    return t1->tv_usec > t2->tv_usec;
}

#define SET_FLAG( x, f )   ({ x |= f; })
#define UNSET_FLAG( x, f ) ({ x &= ~f; })

#define FLAG_EVENT_HANDLER_STYLE 1
#define FLAG_SYSCALL        2
#define FLAG_THREAD_ENTRY   4
#define FLAG_THREAD_CREATE  8

typedef struct _thread_info_t _thread_info_t, *thread_info_t;
struct _thread_info_t
{
    unsigned int   flags;
    /* So many IDs ... */
    OS_THREAD_ID   os_tid, os_ptid;
    THREADID       tid;
    PIN_THREAD_UID utid;
    /* Maybe a little premature optimization. Each thread is in two
     * lists: the one for all threads and either the one for event
     * threads or the one for computation threads.  The latter are
     * mutually exclusive, so we use just one pointer. */
    thread_info_t  next_all, next_ev_comp;
    thread_info_t  parent;
    PIN_SEMAPHORE  sem;
    PIN_MUTEX      mtx;
};

static bool threads_stopped, stop_requested;
static unsigned int event_threads_not_blocked, ev_thread_effort, ev_thread_effort_limit;
static TLS_KEY thread_info;
static FILE * log_file;
static PIN_MUTEX threads_mtx;
static struct timeval ev_thread_timeout;


static THREADID       watchdog_tid = INVALID_THREADID;
static PIN_THREAD_UID watchdog_utid = 0;
static PIN_SEMAPHORE  watchdog_sem;

static thread_info_t get_self( void )
{
    return (thread_info_t)(assert_EF( PIN_GetThreadData(
        thread_info, PIN_ThreadId() ) ) );
}

/* TQ = Thread Queue */
enum thread_kind
{
    ALL_THREAD,
    EV_THREAD,
    COMP_THREAD,
};

namespace TQ
{
    static thread_info_t all_fr, all_bk, ev_fr, ev_bk, comp_fr, comp_bk;

    void init( void )
    {
        all_fr  = NULL;
        all_bk  = NULL;
        ev_fr   = NULL;
        ev_bk   = NULL;
        comp_fr = NULL;
        comp_bk = NULL;
    }

    void front_back( thread_kind k, thread_info_t **f, thread_info_t **b )
    {
        assert_RFL( f && b );
        switch( k )
        {
        case ALL_THREAD:
            *f = &all_fr;
            *b = &all_bk;
            break;
        case EV_THREAD:
            *f = &ev_fr;
            *b = &ev_bk;
            break;
        case COMP_THREAD:
            *f = &comp_fr;
            *b = &comp_bk;
            break;
        default:
            assert_RFL( false );
            break;
        }
    }

#define DECL_FRONT_BACK( k ) \
    thread_info_t *fr, *bk; \
    front_back( k, &fr, &bk );

    thread_info_t *thr_next( thread_kind k, thread_info_t t )
    {
        switch( k )
        {
        case ALL_THREAD:
            return &t->next_all;
        case EV_THREAD:
        case COMP_THREAD:
            return &t->next_ev_comp;
        default:
            break;
        }
        assert_RFL( false );
        return NULL;
    }

#define THR_NEXT( k, t ) ( *thr_next( k, t ) )

    bool is_empty( thread_kind k )
    {
        DECL_FRONT_BACK( k );
        assert_RFL( !(*fr) == !(*bk) );
        return !(*fr);
    }

    thread_info_t peek_front( thread_kind k )
    {
        DECL_FRONT_BACK( k );
        return *fr;
    }

    thread_info_t peek_back( thread_kind k )
    {
        DECL_FRONT_BACK( k );
        return *bk;
    }

    void enqueue( thread_kind k, thread_info_t t )
    {
        DECL_FRONT_BACK( k );
        assert_if_RFL( *fr, *bk );
        assert_if_RFL( !(*fr), !(*bk) );
        assert_RFL( !THR_NEXT( k, t ) );
        if( *fr )
        {
            THR_NEXT( k, *bk ) = t;
            *bk = t;
        }
        else
        {
            *fr = *bk = t;
        }
    }

    void enqueue_front( thread_kind k, thread_info_t t )
    {
        DECL_FRONT_BACK( k );
        assert_if_RFL( *fr, *bk );
        assert_if_RFL( !(*fr), !(*bk) );
        assert_RFL( !THR_NEXT( k, t ) );
        if( *fr )
        {
            THR_NEXT( k, t ) = *fr;
            *fr = t;
        }
        else
        {
            *fr = *bk = t;
        }
    }

    void dequeue( thread_kind k, thread_info_t *t )
    {
        DECL_FRONT_BACK( k );
        assert_if_RFL( *fr, *bk );
        assert_if_RFL( !(*fr), !(*bk) );
        if( t )
        {
            *t = NULL;
        }
        if( fr )
        {
            thread_info_t temp = *fr;
            *fr = THR_NEXT( k, *fr );
            if( !(*fr) )
            {
                *bk = NULL;
            }
            THR_NEXT( k, temp ) = NULL;
            if( t )
            {
                *t = temp;
            }
        }
    }

#undef THR_NEXT
} /* namespace TQ */

#define THR_NEXT( k, t ) ( *TQ::thr_next( k, t ) )

/* XXX: Not really sure what to do with ctx changes yet */
void handle_ctx_change(
    THREADID              threadIndex,
    CONTEXT_CHANGE_REASON reason,
    const CONTEXT        *from,
    CONTEXT              *to,
    INT32                 info,
    VOID                 *v )
{
    LOGF( "idx: %i  CONTEXT CHANGE\n", threadIndex );
}

static bool __attribute__((unused)) is_ev_thread( thread_info_t t )
{
    return t->flags & FLAG_EVENT_HANDLER_STYLE;
}

static bool is_comp_thread( thread_info_t t )
{
    return !( t->flags & FLAG_EVENT_HANDLER_STYLE );
}

/* XXX: Not at all portable */
bool is_thread_create(
    CONTEXT *ctx,
    SYSCALL_STANDARD std )
{
    return
        SYS_clone == PIN_GetSyscallNumber( ctx, std )
        && ( CLONE_THREAD & PIN_GetSyscallArgument( ctx, std, 1 ) );
}

enum run_modes
{
    RUN_FREELY = 0,
    EV_THREAD_WAITING_HINT,  /* be patient for a while ... */
    EV_THREAD_WAITING_FORCE, /* ... then demand all threads pause */
    EV_THREAD_RUNNING,
};

static struct {
    /* run_mode is read _very frequently_ by all threads, so we really
     * don't want any false sharing. */
    int buffer_space1[20];
    run_modes run_mode;
    int buffer_space2[20];
} run_mode_struct;

static run_modes *run_mode_ptr = &run_mode_struct.run_mode;
static unsigned int comp_threads_running = 0, comp_threads_waiting = 0;

/* pre-condition: threads_mtx is held */
static void pause_other_threads( thread_info_t self )
{
    LOGF( "tid: %4lx pause_other\n", PIN_ThreadUid() );
    if( comp_threads_running > 0 )
    {
        LOGF( "tid: %4lx PauseA\n", PIN_ThreadUid() );
        PIN_SemaphoreClear( &self->sem );
        ATOMIC::OPS::Store( run_mode_ptr, EV_THREAD_WAITING_HINT );
        PIN_MutexUnlock( &threads_mtx );
        /* XXX: should probably be a much shorter wait */
        PIN_SemaphoreTimedWait( &self->sem, 1/*milliseconds*/ );
        PIN_MutexLock( &threads_mtx );
        if( comp_threads_running > 0 )
        {
            LOGF( "tid: %4lx PauseB\n", PIN_ThreadUid() );
            PIN_SemaphoreClear( &self->sem );
            ATOMIC::OPS::Store( run_mode_ptr, EV_THREAD_WAITING_FORCE );
            PIN_MutexUnlock( &threads_mtx );
            /* XXX: should probably be a much shorter wait */
            PIN_SemaphoreTimedWait( &self->sem, 1/*milliseconds*/ );
            PIN_MutexLock( &threads_mtx );
        }
        assert_RSL( comp_threads_running == 0 );
    }
    ATOMIC::OPS::Store( run_mode_ptr, EV_THREAD_RUNNING );
}

/* Assumptions:
 * - thread_mtx is held. */
static void resume_other_threads( thread_info_t pauser )
{
    assert_RFL( is_ev_thread( pauser ) );
    assert_RFL( EV_THREAD_RUNNING == ATOMIC::OPS::Load( run_mode_ptr ) );
    LOGF( "tid: %4lx resume  stopped:%i\n", PIN_ThreadUid(), threads_stopped );
    comp_threads_running = comp_threads_waiting;
    comp_threads_waiting = 0;
    ATOMIC::OPS::Store( run_mode_ptr, RUN_FREELY );
    for( thread_info_t t = TQ::peek_front( COMP_THREAD );
         t;
         t = THR_NEXT( COMP_THREAD, t ) )
    {
        PIN_SemaphoreSet( &t->sem );
    }
}

static void handle_syscall_entry(
    THREADID threadIndex,
    CONTEXT *ctx,
    SYSCALL_STANDARD std,
    VOID *v )
{
    PIN_MutexLock( &threads_mtx );
    thread_info_t self = get_self();
    LOGF( "tid: %4lx syscall_entry  q_empty?%i\n", PIN_ThreadUid(), TQ::is_empty( EV_THREAD ) );
    SET_FLAG( self->flags, FLAG_SYSCALL );
    run_modes run_mode = (run_modes)ATOMIC::OPS::Load( run_mode_ptr );

    /* Thread creation gets the "highest priority".  No matter what
     * threads are paused or waiting or whatever, the new thread gets to
     * run next. */
    if( is_thread_create( ctx, std ) )
    {
        SET_FLAG( self->flags, FLAG_THREAD_CREATE );
        PIN_SemaphoreClear( &self->sem );
    }
    /* A computation thread in a syscall doesn't count as "running". */
    else if( is_comp_thread( self ) )
    {
        safe_dec( comp_threads_running );
        if( run_mode != RUN_FREELY
            && comp_threads_running == 0 )
        {
            thread_info_t waiting_ev_thread = TQ::peek_front( EV_THREAD );
            assert_RFL( waiting_ev_thread );
            PIN_SemaphoreSet( &waiting_ev_thread->sem );
        }
    }
    else if( true /* XXX is a blocking syscall */ )
    {
        assert_RFL( TQ::peek_front( EV_THREAD ) == self );
        TQ::dequeue( EV_THREAD, NULL );
        thread_info_t next = TQ::peek_front( EV_THREAD );
        if( next )
        {
            /* XXX: EV threads can collectively starve comp threads.
             * Problem? */
            assert_EF( !gettimeofday( &ev_thread_timeout, NULL ) );
            ev_thread_timeout.tv_usec += 1000;
            if( ev_thread_timeout.tv_usec > 1000000 )
            {
                ev_thread_timeout.tv_usec -= 1000000;
                ev_thread_timeout.tv_sec += 1;
            }
            PIN_SemaphoreSet( &next->sem );
        }
        else if( 0 < comp_threads_waiting )
        {
            resume_other_threads( self );
            PIN_SemaphoreSet( &watchdog_sem );
        }
    }
    /* Otherwise, we're going to assume the syscall runs quickly and
     * treat it more like regular computation.  */
    PIN_MutexUnlock( &threads_mtx );
}

VOID handle_syscall_exit(
    THREADID threadIndex,
    CONTEXT *ctx,
    SYSCALL_STANDARD std,
    VOID *v)
{
    PIN_MutexLock( &threads_mtx );
    LOGF( "tid: %4lx syscall_exit\n", PIN_ThreadUid() );
    if( is_thread_create( ctx, std )
        && ( 0 > ((int)PIN_GetSyscallReturn( ctx, std )) ) )
    {
        thread_info_t self = get_self();
        UNSET_FLAG( self->flags, FLAG_THREAD_CREATE );
    }
    PIN_MutexUnlock( &threads_mtx );
}

void analyze_post_syscall( void )
{
    PIN_MutexLock( &threads_mtx );
    thread_info_t self = get_self();
    LOGF( "tid: %4lx post_syscall\n", PIN_ThreadUid() );
    assert_RFL( !THR_NEXT( EV_THREAD, self ) );
    if( !( self->flags & FLAG_SYSCALL ) )
    {
        PIN_MutexUnlock( &threads_mtx );
        return;
    }
    UNSET_FLAG( self->flags, FLAG_SYSCALL );
    /* If we just created a new thread, we must wait. */
    if( self->flags & FLAG_THREAD_CREATE )
    {
        PIN_MutexUnlock( &threads_mtx );
        PIN_SemaphoreWait( &self->sem );
        PIN_MutexLock( &threads_mtx );
    }
    UNSET_FLAG( self->flags, FLAG_THREAD_CREATE );
    if( is_comp_thread( self ) )
    {
        run_modes run_mode = (run_modes)ATOMIC::OPS::Load( run_mode_ptr );
        if( run_mode == RUN_FREELY )
        {
            safe_inc( comp_threads_running );
            PIN_MutexUnlock( &threads_mtx );
        }
        else
        {
            safe_inc( comp_threads_waiting );
            PIN_SemaphoreClear( &self->sem );
            PIN_MutexUnlock( &threads_mtx );
            PIN_SemaphoreWait( &self->sem );
        }
    }
    else if( TQ::is_empty( EV_THREAD ) )
    {
        /* Nobody else is trying to go.  Just do it. */
        TQ::enqueue( EV_THREAD, self );
        pause_other_threads( self );
        PIN_MutexUnlock( &threads_mtx );
    }
    else if( TQ::peek_front( EV_THREAD ) == self )
    {
        /* Decided to not switch for syscall */
        pause_other_threads( self );
        PIN_MutexUnlock( &threads_mtx );
    }
    else
    {
        TQ::enqueue( EV_THREAD, self );
        PIN_SemaphoreClear( &self->sem );
        PIN_MutexUnlock( &threads_mtx );
        PIN_SemaphoreWait( &self->sem );
    }
}

static thread_info_t find_parent( thread_info_t self )
{
    thread_info_t t = TQ::peek_front( ALL_THREAD );
    if( !t )
    {
        /* This must be the original thread */
        return NULL;
    }
    /* Could definitely do better than linear, but this shouldn't be
     * called often, so it probably doesn't matter. */
    while( t )
    {
        if( self->os_ptid == t->os_tid )
        {
            return t;
        }
        t = THR_NEXT( ALL_THREAD, t );
    }
    assert_RFL( false );
    return NULL;
}

static void handle_thread_start(
    THREADID threadIndex,
    CONTEXT *ctx,
    INT32    flags,
    VOID    *v )
{
    PIN_MutexLock( &threads_mtx );
    thread_info_t self = (thread_info_t)assert_EF( malloc( sizeof( self[0] ) ) );
    LOGF( "tid: %4lx thread_start\n", PIN_ThreadUid() );
    self->os_tid       = PIN_GetTid();
    self->os_ptid      = PIN_GetParentTid();
    self->tid          = PIN_ThreadId();
    self->utid         = PIN_ThreadUid();
    self->flags        = FLAG_EVENT_HANDLER_STYLE | FLAG_THREAD_ENTRY;
    self->next_all     = NULL;
    self->next_ev_comp = NULL;
    self->parent       = find_parent( self );
    assert_EF( PIN_SemaphoreInit( &self->sem ) );
    assert_EF( PIN_MutexInit( &self->mtx ) );

    TQ::enqueue( ALL_THREAD, self );
    assert_EF( PIN_SetThreadData(
                thread_info, self, PIN_ThreadId() ) );
    PIN_MutexUnlock( &threads_mtx );
}

void analyze_thread_entry( void )
{
    PIN_MutexLock( &threads_mtx );
    thread_info_t self = get_self();
    LOGF( "tid: %4lx thread_entry\n", PIN_ThreadUid() );
    if( !( self->flags & FLAG_THREAD_ENTRY ) )
    {
        /* Weird case where a thread entry point executes "again". */
        PIN_MutexUnlock( &threads_mtx );
        return;
    }
    pause_other_threads( self );
    UNSET_FLAG( self->flags, FLAG_THREAD_ENTRY );
    // ++event_threads_not_blocked;
    TQ::enqueue_front( EV_THREAD, self );
    LOGF( "tid: %4lx   q_empty?%i\n", PIN_ThreadUid(), TQ::is_empty( EV_THREAD ) );
    PIN_MutexUnlock( &threads_mtx );
}

/* trace_if must be extremely fast, because it is called before the
 * execution of almost every trace.  (The only exceptions are the traces
 * that are handled specially, like thread entry and syscalls.)
 * trace_if should return 0 (RUN_FREELY) the vast majority of the time
 * for applications that are at all CPU-intensive. */
static ADDRINT trace_if( void )
{
    return ATOMIC::OPS::Load( run_mode_ptr );
}

static void pause_self()
{
    PIN_MutexLock( &threads_mtx );
    thread_info_t self = get_self();
    assert_RFL( is_comp_thread( self ) );
    safe_dec( comp_threads_running );
    safe_inc( comp_threads_waiting );
    if( comp_threads_running == 0 )
    {
        thread_info_t waiting_ev_thread = TQ::peek_front( EV_THREAD );
        assert_RFL( waiting_ev_thread );
        PIN_SemaphoreSet( &waiting_ev_thread->sem );
    }
    PIN_SemaphoreClear( &self->sem );
    PIN_MutexUnlock( &threads_mtx );
    PIN_SemaphoreWait( &self->sem );
}

/* trace_then is called when an EV thread is waiting to run or running. */
static void trace_then( UINT32 sz )
{
    run_modes run_mode = (run_modes)ATOMIC::OPS::Load( run_mode_ptr );
    switch( run_mode )
    {
    case RUN_FREELY:
        /* I don't think it's possible for run_mode to change like this
         * between trace_if and trace_then, but maybe it is. */
        return;
    case EV_THREAD_WAITING_HINT:
        if( false /* XXX at safe pause point */ )
        {
            pause_self();
        }
        break;
    case EV_THREAD_WAITING_FORCE:
        pause_self();
        break;
    case EV_THREAD_RUNNING:
        ev_thread_effort += sz;
        if( ev_thread_effort > ev_thread_effort_limit )
        {
            struct timeval curr_time;
            assert_EF( !gettimeofday( &curr_time, NULL ) );
            if( tv_greater_than( &curr_time, &ev_thread_timeout ) )
            {
                /* Not an EV thread anymore! */
            }
            else
            {
                ev_thread_effort_limit = ev_thread_effort + 1000;
            }
        }
        break;
    default:
        assert_RFL( false );
    }
    // if( self ){}
}

static void instrument_trace( TRACE trace, VOID *v )
{
    PIN_MutexLock( &threads_mtx );
    thread_info_t self = get_self();
    int entry   = !!( self->flags & FLAG_THREAD_ENTRY ),
        syscall = !!( self->flags & FLAG_SYSCALL );
    assert_RFL( !( entry && syscall ) );
    if( entry )
        TRACE_InsertCall( trace, IPOINT_BEFORE, (AFUNPTR)analyze_thread_entry, IARG_END );
    else if( syscall )
        TRACE_InsertCall( trace, IPOINT_BEFORE, (AFUNPTR)analyze_post_syscall, IARG_END );
    else
    {
        /* Common case.  Use if/then instrumentation for better performance. */
        /* "sz" is a _very_ rough approximation of the execution cost of a trace */
        UINT32 sz = (UINT32)TRACE_Size( trace );
        /* TODO: decide if this is a safe pause point */
        TRACE_InsertIfCall  ( trace, IPOINT_BEFORE, (AFUNPTR)trace_if,   IARG_END );
        TRACE_InsertThenCall( trace, IPOINT_BEFORE, (AFUNPTR)trace_then,
                              IARG_UINT32, sz, IARG_END );
    }
    PIN_MutexUnlock( &threads_mtx );
}

void handle_thread_fini(
    THREADID threadIndex,
    const CONTEXT *ctx,
    INT32 code,
    VOID *v )
{
    PIN_MutexLock( &threads_mtx );
    LOGF( "tid: %x thread_fini\n", threadIndex );
    thread_info_t self = get_self();
    PIN_SemaphoreFini( &self->sem );
    free( self );
    // unsigned int temp = event_threads_not_blocked;
    /* XXX hm... do threads need to make a syscall to fini */
    // --event_threads_not_blocked;
    // LOGF( "N D ready %u  %u\n", self->tid, event_threads_not_blocked );
    // assert_RFL( event_threads_not_blocked < temp );
    assert_EF( PIN_SetThreadData( thread_info, NULL, PIN_ThreadId() ) );
    PIN_MutexUnlock( &threads_mtx );
}

VOID Fini(INT32 code, VOID *v)
{
    LOGF( "#eof\n" );
    fclose( log_file );
    assert_EF( PIN_DeleteThreadDataKey( thread_info ) );
    PIN_MutexFini( &threads_mtx );
}


/* ===================================================================== */
/* Print Help Message                                                    */
/* ===================================================================== */

INT32 Usage()
{
    PIN_ERROR("This tool does some thread shit" 
                + KNOB_BASE::StringKnobSummary() + "\n");
    return -1;
}


void watchdog( void *a )
{
#if 0
    while( 1 )
    {
        if( PIN_IsProcessExiting() )
        {
            break;
        }
        PIN_MutexLock( &threads_mtx );
        // LOGF( "watchdog loop\n" );
        if( need_resume )
        {
            resume_other_threads( need_resume );
            PIN_SemaphoreSet( &need_resume->sem );
            need_resume = NULL;
        }
        else if( TQ::is_empty( EV_THREAD ) )
        {
            /* No event threads are ready to run.  Wait on the
             * semaphore.  We need a timeout to avoiding hanging on
             * process termination. */
            PIN_SemaphoreClear( &watchdog_sem );
            PIN_MutexUnlock( &threads_mtx );
            BOOL is_set = PIN_SemaphoreTimedWait(
                &watchdog_sem, 100/*millisecond*/ );
            PIN_MutexLock( &threads_mtx );
            if( is_set )
                PIN_SemaphoreClear( &watchdog_sem );
        }
        else
        {
            struct timespec sleep_time;
            sleep_time.tv_sec  = 0;
            sleep_time.tv_nsec = 100000;
            unsigned int old_event_count = event_count;
            PIN_MutexUnlock( &threads_mtx );
            while( nanosleep( &sleep_time, &sleep_time ) ) { }
            PIN_MutexLock( &threads_mtx );
            if( old_event_count == event_count )
            {
                /* The currently executing thread is not an event
                 * thread. */
            }
        }
        PIN_MutexUnlock( &threads_mtx );
    }
#endif
}

/* ===================================================================== */
/* Main                                                                  */
/* ===================================================================== */

int main(int argc, char *argv[])
{
    if( PIN_Init( argc, argv ) ) return Usage();

    log_file = fopen( "strace.out", "w" );
    thread_info = PIN_CreateThreadDataKey( NULL );
    assert_EF( PIN_MutexInit( &threads_mtx ) );
    event_threads_not_blocked = 0;
    TQ::init();
    threads_stopped = false;
    stop_requested  = false;
    // need_resume = NULL;

    PIN_AddThreadStartFunction  ( handle_thread_start,  NULL );
    PIN_AddThreadFiniFunction   ( handle_thread_fini,   NULL );
    PIN_AddSyscallEntryFunction ( handle_syscall_entry, NULL );
    PIN_AddSyscallExitFunction  ( handle_syscall_exit,  NULL );
    PIN_AddContextChangeFunction( handle_ctx_change,    NULL );
    TRACE_AddInstrumentFunction ( instrument_trace,     NULL );
    PIN_AddFiniFunction         (Fini, 0);

    assert_EF( PIN_SemaphoreInit( &watchdog_sem ) );
    watchdog_tid = PIN_SpawnInternalThread(
        watchdog, NULL, 0, &watchdog_utid );
    assert_RFL( watchdog_tid != INVALID_THREADID );

    // Never returns
    PIN_StartProgram();
    
    return 0;
}
