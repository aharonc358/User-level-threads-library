
#include <queue>
#include <stdio.h>
#include <setjmp.h>
#include <signal.h>
#include <unistd.h>
#include <sys/time.h>
#include <stdbool.h>
#include <cstdlib>
#include "Tcb.h"
#include "uthreads.h"

typedef unsigned long address_t;
#define JB_SP 6
#define JB_PC 7
#define  QUNTUM_NRGATIVE  "quantum_usecs must be positive"
#define THREAD_INVALID  "the thread id is invalid "
#define ALLOC_MEMORY_FAIL "allocation memory fail"
#define TIMER_FAILED "timer system call fail"
#define MASK_FAILED "system call fail in masking"
#define MAX_THREAD_LIMITED "you have max threads"
#define ENTRY_POINT_NULLPTR  "entry point mast be non nullptr"
#define SYSCALL_TIMER_FAILED "the system call setitimer fail "
#define INVALID_TID "tid is invalid "

std::string SYSTEM_ERROR  = "system error: ";
std::string THREAD_ERROR  = "thread library error: ";

//array of the threads
Tcb* tcb_array[MAX_THREAD_NUM];
//the resident thread
Tcb* current_tcb;

struct sigaction sa_for_timer = {0};
struct itimerval timer;

std::priority_queue<int> id_available;
MyQueue readyQueue;
int quantum_usecs_for_thread;
int total_num_of_quantums = 1;
min_heap_pairs wake_heap;
bool end_run = false;


//translate adress for sp and pc
address_t translate_address(address_t addr)
{
  address_t ret;
  asm volatile("xor    %%fs:0x30,%0\n"
               "rol    $0x11,%0\n"
      : "=g" (ret)
      : "0" (addr));
  return ret;
}

void setup_thread(Tcb* thread, thread_entry_point entry_point)
{
//  adjust personal Stack pointer and program counter for the thread
  address_t sp = (address_t) thread->stack + STACK_SIZE - sizeof (address_t);
  address_t pc = (address_t) entry_point;
//saves the current stack environment in the jmp_buf structure and optionally
// saves the current signal mask.
  sigsetjmp(thread->env, 1);
  (thread->env->__jmpbuf)[JB_SP] = translate_address (sp);
  (thread->env->__jmpbuf)[JB_PC] = translate_address (pc);
//  empty the set of masks
  sigemptyset (&thread->env->__saved_mask);
}

int set_timer();


void free_all(){
  end_run = true;
  for (int i = 0; i < MAX_THREAD_NUM; i ++){
    if (tcb_array[i] != nullptr) delete tcb_array[i];
  }
}


//masking functoion for timer signal, for not having a sudden context switch
// within a critival actions. The function checks if it can mask the timer
// signal.
void mask_timer_for_tcb(Tcb* tcb)
{
  sigset_t set;
  if (sigemptyset(&set) == -1 || sigaddset(&set, SIGVTALRM) == -1 ||
      sigprocmask (SIG_BLOCK, &set, &tcb->old_mask) == -1)
  {
    std::cerr << SYSTEM_ERROR << MASK_FAILED << std::endl;
    free_all();
    exit (1);
  }
}

//the function checks if it can unmask the timer signal. It does it only
// after the critical action has ended and context switch is available.
void unmask_timer_for_tcb(Tcb* tcb)
{
  if (sigprocmask (SIG_SETMASK, &tcb->old_mask, nullptr)== -1){

    std::cerr << SYSTEM_ERROR << MASK_FAILED <<std::endl;
    free_all();
    exit (1);
  }
}


void update_quantums()
{
//  updates the quantums of the resident thread, and (if the blocked heap is
//  not empty) pushes the first thread in the heap to the ready queue
  current_tcb->num_of_quantums += 1;
  total_num_of_quantums += 1;
  while (wake_heap.empty () == false && wake_heap.top().first ==
                                        total_num_of_quantums)
  {
    std::pair<int, int> wake_up = wake_heap.top ();
    wake_heap.pop ();
    if (tcb_array[wake_up.second] != nullptr
        && tcb_array[wake_up.second]->is_blocked == false)
    {
      tcb_array[wake_up.second]->is_sleeping = false;
      readyQueue.push (tcb_array[wake_up.second]);
    }
  }

}


void switch_thread(bool save_to_ready, bool save_cur_env)
{
  if (readyQueue.empty ()){
    update_quantums();
    return ;
  }
//  the threads to be swapped
  Tcb *old_tcb = current_tcb;
  Tcb *new_tcb = readyQueue.front ();
  readyQueue.pop ();
//  if we want to preserve the resident threads env, we will use sigsetjmp
//  to save the value. if it failed(retuned with 0), return.
  if (save_cur_env)
  {
    int ret_val = sigsetjmp(old_tcb->env, 1);
    bool did_just_save_bookmark = ret_val == 0;
    if (!did_just_save_bookmark) return;
  }
  current_tcb = new_tcb;
  if (save_to_ready) readyQueue.push (tcb_array[old_tcb->id]);
  if (set_timer ()< 0)
  {
    std::cerr << SYSTEM_ERROR << SYSCALL_TIMER_FAILED <<
              std::endl;
    free_all();
    exit (1);
  }
//update the quantom of the resident thread.
  update_quantums();
//  restores the environment saved by env
  siglongjmp (new_tcb->env, 1);
}



void timer_handler(int sig)
{
  if (end_run) return;
  mask_timer_for_tcb (current_tcb);
  switch_thread(true, true);
  unmask_timer_for_tcb (current_tcb);
}

int init_timer(){
  sa_for_timer.sa_handler = &timer_handler;
  if (sigaction(SIGVTALRM, &sa_for_timer, NULL) < 0) return -1;

  timer.it_value.tv_sec = 0;        // first time interval, seconds part
  timer.it_value.tv_usec = quantum_usecs_for_thread;        // first time interval, microseconds part

  // configure the timer to expire every 3 sec after that.
  timer.it_interval.tv_sec = 0;    // following time intervals, seconds part
  timer.it_interval.tv_usec = quantum_usecs_for_thread;
  return 0;
}

int set_timer(){

  if (setitimer(ITIMER_VIRTUAL, &timer, NULL)) {
    return -1;
  }
  return 0;

}


int uthread_init(int quantum_usecs)
{
  if (quantum_usecs <= 0)
  {//its an error to have a negative quantum
    std::cerr << THREAD_ERROR <<QUNTUM_NRGATIVE  << std::endl;
    return -1;
  }
//  init the min heap(a max heap(because this is the deafault heap in c++)
//  with negative values so we could pop easily
//  the minimum value within the heap(which is also the greatest negative
//  number)
  for (int i = 1; i < MAX_THREAD_NUM; i++)
  {
    id_available.push (-i);
  }
//  init the main thread (id = 0)
  Tcb *mainTcb = new (std::nothrow) Tcb(0);
  if (mainTcb == nullptr){
    std::cerr << SYSTEM_ERROR<<ALLOC_MEMORY_FAIL << std::endl;
    exit (1);
  }
  tcb_array[0] = mainTcb;
  current_tcb = mainTcb;
  mainTcb->num_of_quantums = 1;
  quantum_usecs_for_thread = quantum_usecs;
  if ((init_timer() < 0) || (set_timer() < 0)){
    std::cerr  << SYSTEM_ERROR <<TIMER_FAILED << std::endl;
    delete mainTcb;
    exit (1);
  }
  return 0;
}

int uthread_spawn(thread_entry_point entry_point)
{
  mask_timer_for_tcb (current_tcb);
//  checks if there are still available left id's to give, which mean it
//  checks if there are no more threads than the limited number.
  if (id_available.empty()){
    std::cerr  << THREAD_ERROR << MAX_THREAD_LIMITED<< std::endl;
    unmask_timer_for_tcb (current_tcb);
    return -1;
  }
//  checks validity of entry point.
  if (entry_point == nullptr){
    std::cerr  << THREAD_ERROR <<ENTRY_POINT_NULLPTR << std::endl;
    unmask_timer_for_tcb (current_tcb);
    return -1;
  }
//  the maximum negative number is the minimum positive number.Thus, we can
//  find the smallest positive available number there is in the heap.
  int id = -id_available.top();
  id_available.pop();
  Tcb *thread = new (std::nothrow) Tcb(id);
  if (thread == nullptr)
  {
    free_all();
    std::cerr  << SYSTEM_ERROR << ALLOC_MEMORY_FAIL << std::endl;
    exit (1);
  }
//  update the thread array with the new thread in the last place.
  tcb_array[id] = thread;
  setup_thread (thread, entry_point);
//  push the constructed thread to the end of the ready queue as instructed
  readyQueue.push (tcb_array[id]);
//  the critical task finished, so we can stop the masking adn allow context
//  switch
  unmask_timer_for_tcb (current_tcb);
  return id;
}

int uthread_terminate(int tid)
{
//  mask timer signal for not having a context switch within the critical
//  action
  mask_timer_for_tcb (current_tcb);
//  check validity
  if (tid < 0 || tid >= MAX_THREAD_NUM || tcb_array[tid] == nullptr)
  {
    std::cerr  << THREAD_ERROR << THREAD_INVALID << std::endl;
    unmask_timer_for_tcb (current_tcb);
    return -1;
  }
  if (tid == 0)
  {
    free_all();
    exit(0);
  }
//  checks the id of the thread to be terminated, and terminate it. add back
//  to the available heap
  bool terminate_current_thread = current_tcb->id == tid;
  readyQueue.remove (tcb_array[tid]);
  delete tcb_array[tid];
  tcb_array[tid] = nullptr;
  id_available.push (-tid);
//  if it wants to terminate itself, dont save the env and dont push back to
//  the ready queue
  if (terminate_current_thread)
  {
    switch_thread (false, false);
  }
//  unmask the timer signal masking, for allowing from now context switch
  unmask_timer_for_tcb (current_tcb);
  return 0;
}

int uthread_block(int tid)
{
  mask_timer_for_tcb (current_tcb);
  //tid cant be negative and cant have tid such that greater then the max.
  if (tid <= 0 || tid >= MAX_THREAD_NUM || tcb_array[tid] == nullptr)
  {
    std::cerr  << THREAD_ERROR << INVALID_TID << std::endl;
    unmask_timer_for_tcb (current_tcb);
    return -1;
  }
//  block and remove thread from the ready queue.if the blocked thread is
//  itself, save its env
  tcb_array[tid]->is_blocked = true;
  readyQueue.remove (tcb_array[tid]);
  if (current_tcb->id == tid){
    switch_thread (false, true);
  }
  unmask_timer_for_tcb (current_tcb);
  return 0;
}

int uthread_resume(int tid)
{
  mask_timer_for_tcb (current_tcb);
  if (tid < 0 || tid >= MAX_THREAD_NUM || tcb_array[tid] == nullptr)
  {
    std::cerr  << THREAD_ERROR << INVALID_TID << std::endl;
    unmask_timer_for_tcb (current_tcb);
    return -1;
  }
//  check if its blocked
  if (tcb_array[tid]->is_blocked == false){
    unmask_timer_for_tcb (current_tcb);
    return 0;
  }
//  cancel the block of the thread, and check sleeping and push.
  tcb_array[tid]->is_blocked = false;
  if (tcb_array[tid]->is_sleeping == false){
    readyQueue.push (tcb_array[tid]);
  }
  unmask_timer_for_tcb (current_tcb);
  return 0;
}

int uthread_get_tid(){

  return current_tcb->id;
}



int uthread_get_total_quantums(){
  return total_num_of_quantums;
}

int uthread_get_quantums(int tid)
{
  mask_timer_for_tcb (current_tcb);
  if (tid < 0 || tid >= MAX_THREAD_NUM || tcb_array[tid] == nullptr)
  {
    std::cerr  << THREAD_ERROR << INVALID_TID << std::endl;
    unmask_timer_for_tcb (current_tcb);
    return -1;
  }
  unmask_timer_for_tcb (current_tcb);
  return tcb_array[tid]->num_of_quantums;
}



int uthread_sleep(int num_quantums)
{
  mask_timer_for_tcb (current_tcb);
  //cant send the main thread to sleep
  if (current_tcb->id == 0){
    std::cerr  << THREAD_ERROR << INVALID_TID << std::endl;
    unmask_timer_for_tcb (current_tcb);
    return -1;
  }
  current_tcb->is_sleeping = true;
  wake_heap.push (std::make_pair (num_quantums+total_num_of_quantums,
                                  current_tcb->id));
  switch_thread (false, true);
  unmask_timer_for_tcb (current_tcb);
  return 0;
}