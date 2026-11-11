
#include "syslib.h"
#include <minix/com.h>
#include <string.h>

int sys_schedule(endpoint_t proc_ep,
                 int priority,
                 int quantum,
                 int cpu)
{
    message m;

    memset(&m, 0, sizeof(m));

    m.SCHEDULING_ENDPOINT = proc_ep;
    m.SCHEDULING_PRIORITY = priority;
    m.SCHEDULING_QUANTUM = quantum;
    m.SCHEDULING_CPU = cpu;
    m.SCHEDULING_TICKETS = -1;

    return _kernel_call(SYS_SCHEDULE, &m);
}

int sys_schedule_tickets(endpoint_t proc_ep,
                         int priority,
                         int quantum,
                         int cpu,
                         int tickets)
{
    message m;

    memset(&m, 0, sizeof(m));

    m.SCHEDULING_ENDPOINT = proc_ep;
    m.SCHEDULING_PRIORITY = priority;
    m.SCHEDULING_QUANTUM = quantum;
    m.SCHEDULING_CPU = cpu;
    m.SCHEDULING_TICKETS = tickets;

    return _kernel_call(SYS_SCHEDULE, &m);
}
