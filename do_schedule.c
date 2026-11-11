#include "kernel/system.h"
#include <minix/endpoint.h>
#include <minix/u64.h>

/*===========================================================================*
 *                              do_schedule                                  *
 *===========================================================================*/
int do_schedule(struct proc *caller, message *m_ptr)
{
    int rv;
    long tickets;
    struct proc *p;
    int proc_nr;
    int priority, quantum, cpu;

    if (!isokendpt(m_ptr->SCHEDULING_ENDPOINT, &proc_nr))
        return EINVAL;

    p = proc_addr(proc_nr);

    /* Only this process' scheduler can schedule it */
    if (caller != p->p_scheduler)
        return EPERM;

    priority = (int) m_ptr->SCHEDULING_PRIORITY;
    quantum = (int) m_ptr->SCHEDULING_QUANTUM;
    cpu = (int) m_ptr->SCHEDULING_CPU;
    tickets = m_ptr->SCHEDULING_TICKETS;

    if (tickets < -1 || tickets > SCHEDULING_MAX_TICKETS)
        return EINVAL;

    rv = sched_proc(p, priority, quantum, cpu);

    if (rv != OK)
        return rv;

    /* -1 significa que a quantidade de tickets não deve ser alterada. */
    if (tickets != -1) {
        p->p_lottery_tickets = (unsigned) tickets;

        /* Reinicia as estatísticas após uma nova atribuição. */
        p->p_lottery_wins = 0;
        p->p_context_switches_in = 0;
        p->p_context_switches_out = 0;
        p->p_quantum_expirations = 0;

        make_zero64(p->p_lottery_cycles);
    }

    return OK;
}