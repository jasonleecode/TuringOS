#include "task_table.h"

#include <l4/re/env>
#include <l4/re/util/cap_alloc>
#include <l4/sys/ipc.h>
#include <cstring>
#include <cstdio>

Task_table::Task_table() : _next_handle(1)
{
    memset(_tasks, 0, sizeof(_tasks));
    for (int i = 0; i < MAX; i++)
        _tasks[i].state = Child_task::FREE;
}

Child_task *Task_table::alloc(const char *name)
{
    for (int i = 0; i < MAX; i++) {
        if (_tasks[i].state != Child_task::FREE)
            continue;

        _tasks[i].handle    = _next_handle++;
        _tasks[i].state     = Child_task::RUNNING;
        _tasks[i].exit_code = 0;
        snprintf(_tasks[i].name, sizeof(_tasks[i].name), "%s", name ? name : "?");
        return &_tasks[i];
    }
    return nullptr;
}

Child_task *Task_table::find(l4_uint32_t handle)
{
    for (int i = 0; i < MAX; i++) {
        if (_tasks[i].state != Child_task::FREE &&
            _tasks[i].handle == handle)
            return &_tasks[i];
    }
    return nullptr;
}

void Task_table::set_exited(l4_uint32_t handle, long code)
{
    Child_task *t = find(handle);
    if (!t) return;
    t->state     = Child_task::EXITED;
    t->exit_code = code;
}

void Task_table::free(l4_uint32_t handle)
{
    Child_task *t = find(handle);
    if (!t) return;

    /* Unique_del_cap RAII destroys the kernel objects on reset. */
    t->task.reset();
    t->thread.reset();
    t->rm.reset();
    t->parent_gate.reset();

    t->state  = Child_task::FREE;
    t->handle = 0;
    t->name[0] = '\0';
}

int Task_table::count() const
{
    int n = 0;
    for (int i = 0; i < MAX; i++)
        if (_tasks[i].state != Child_task::FREE) n++;
    return n;
}

Child_task *Task_table::at(int index)
{
    if (index < 0 || index >= MAX) return nullptr;
    return &_tasks[index];
}
