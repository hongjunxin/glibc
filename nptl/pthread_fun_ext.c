/* Created by HongJunxin 2019.

   The GNU C Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   The GNU C Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with the GNU C Library.  If not, see
   <http://www.gnu.org/licenses/>.  */


#include "pthreadP.h"
#include <stdio.h>
#include <errno.h>
#include <dirent.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>


/* here type of tid is pid_t. */
static int comm_to_tid(char *name);
static pid_t pthread_t_to_tid(pthread_t pthread);
static int read_line(int fd, char *buf, unsigned int size);
static void tid_to_comm(int tid, char *comm, unsigned int size);
static void printf_stack_info(struct pthread *pd);
static void *fn_ext(void *arg);


static unsigned int suspend_tasks = 0;
static pthread_mutex_t suspend_tasks_mtx = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * Description
 * 		Get thread (task) id according to thread name.
 * Parameter
 * 		name: thread (task) name
 * 		pthread: buffer to save thread id 
 * Return
 *		0 		on success
 *		EINVAL	if pthread is error
 *		ESRCH	if thread (task) not found according to name
 *		ETYPENOTMATCH	task type not match			
 */	
int pthread_getid(char *name, pthread_t *pthread)
{	
	int tid, fd;
	char path[64] = {'\0'};
	char file[64] = {'\0'};
	char pthread_t_str[64] = {'\0'};
	DIR *pid_dir = NULL;
	struct dirent *entry;

	if (name == NULL || pthread == NULL) 
		return EINVAL;	

	tid = comm_to_tid(name);
	if (tid == -1)
		return ESRCH;

	snprintf(path, sizeof(path), "/var/%d%s", (int) getpid(), VAR_PIDDIR_SUFFIX);
	if ((pid_dir = opendir(path)) == NULL)
		return ESRCH;	
	while ((entry = readdir(pid_dir))) {
		if (entry->d_type == DT_REG) {
			snprintf(file, sizeof(file), "%s/%s", path, entry->d_name);
			if ((fd = open(file, O_RDONLY)) == -1)
				continue;  /* go ahead. maybe will find a better one. */	
			if ((read_line(fd, pthread_t_str, sizeof(pthread_t_str))) == -1)
				continue;			
			if (pthread_t_to_tid((pthread_t) atol(pthread_t_str)) == tid) {
				*pthread = (pthread_t) atol(pthread_t_str);
				return 0;
			}
		}		
	}
	closedir(pid_dir);
	
	return ESRCH;
}

static int comm_to_tid(char *name)
{	
	int fd;
	int ret = -1;
	DIR *task = NULL;
	struct dirent *entry;
	char pid_task_dir[64] = {'\0'};
	char comm_path[64] = {'\0'};
	char comm[64] = {'\0'};
	
	if (name == NULL) 
		return -1;		
			
	snprintf(pid_task_dir, sizeof(pid_task_dir), "/proc/%d/task", (int)getpid());	
	if ((task = opendir(pid_task_dir)) == NULL)
		return -1;
		
	while ((entry = readdir(task))) {
		if (entry->d_type == DT_DIR) {
			snprintf(comm_path, sizeof(comm_path), "%s/%s/comm", pid_task_dir, entry->d_name);
			if ((fd = open(comm_path, O_RDONLY)) == -1) 
				continue;  /* go ahead. maybe will find a better one. */			
			ret = read_line(fd, comm, sizeof(comm));
			close(fd);
			if (ret == -1)
				continue;
			if (strncmp(name, comm, strlen(comm)) == 0) {
				ret = atoi(entry->d_name);
				break;
			}
		}
	}
	closedir(task);		

	return ret;
}

static int pthread_t_to_tid(pthread_t pthread)
{
	struct pthread *pd;	
	pd = (struct pthread *) pthread;

	if (INVALID_TD_P (pd))
		return -1;	/* Not a valid thread handle. */
	
	return (int)pd->tid;
}

static int read_line(int fd, char *buf, unsigned int size)
{
	char *c;
	int i;

	c = (char *) malloc(1);	
	memset(buf, '\0', size);

	if (buf == NULL)
		return -1;
	
	for (i=0; i<size; i++) {
		if (read(fd, c, 1) != 1 || (*c == '\n') || (*c == '\0')) {
			buf[i] = '\0';		
			break;
		} else {
			buf[i] = *c;
		}		
	}

	if (i == size)
		buf[i-1] = '\0';

	free(c);
	return 0;
}

/*
 * Description
 *		To know whether pthread is in ready or not.
 * Return
 *		TRUE	in ready
 *		FALSE	not in ready or pthread not exist.
 *				set errno to ESRCH if pthread not exist
 */
boolean pthread_is_ready(pthread_t pthread)
{
	char path[64] = {'\0'};
	char stat[64] = {'\0'};
	char *c;
	int fd;
	struct pthread *pd = (struct pthread *) pthread;

	/* Make sure the descriptor is valid. */
	if (INVALID_TD_P (pd)) {	
		errno = ESRCH;  /* Not a valid thread handle. */
		return FALSE;
	}
	
	snprintf(path, sizeof(path), "/proc/%d/task/%d/stat", pd->pid, pd->tid);
	if ((fd = open(path, O_RDONLY)) == -1)
		return FALSE;
	read(fd, stat, sizeof(stat));
	close(fd);
	
	for (c=stat; *c!='\0'; c++) {
		if (*c != ')') 
			continue;
		c++; /* go to space */
		c++; /* go to flag */
		if (*c == 'R')
			return TRUE;
		else
			return FALSE;
	}
	return FALSE;
}


/*
 * Description
 *		To know whether pthread is in suspend
 * Return
 *		TRUE	in suspend
 *		FALSE	not in suspend or pthread not exist.
 *				set errno to ESRCH if pthread not exist
 */
boolean pthread_is_suspend(pthread_t pthread)
{
	char path[64] = {'\0'};
	char stat[64] = {'\0'};
	char *c;
	int fd;
	struct pthread *pd = (struct pthread *) pthread;

	/* Make sure the descriptor is valid. */
	if (INVALID_TD_P (pd)) {		
		errno = ESRCH;  /* Not a valid thread handle. */
		return FALSE;
	}	
	
	snprintf(path, sizeof(path), "/proc/%d/task/%d/stat", pd->pid, pd->tid);
	if ((fd = open(path, O_RDONLY)) == -1)
		return FALSE;
	read(fd, stat, sizeof(stat));
	close(fd);
	
	for (c=stat; *c!='\0'; c++) {
		if (*c != ')') 
			continue;
		c++; /* go to space */
		c++; /* go to flag */
		if (*c == 'S')
			return TRUE;
		else
			return FALSE;
	}
	return FALSE;
}


/*
 * Description
 *		If the caller thread is in R status other any threads in
 *		the same process can't get running. This API support recursive 
 *		call.
 * Parameter
 *		void
 * Return
 *		0	on success
 *		-1  on error
 */
int pthread_lock(void)
{
	char path[64] = {'\0'};
	DIR *dir = NULL;
	struct dirent *entry;
	pthread_t pt;
	pid_t tid;
	cpu_set_t set;
	struct sched_param param;
	struct pthread *pd = THREAD_SELF;
	
	if (pd->pthread_lock_counter > 0) {
		if (pd->pthread_lock_counter >= UINT_MAX)
			return -1;		
		++pd->pthread_lock_counter;		
		return 0;
	} else {
		++pd->pthread_lock_counter;		
	}

	__pthread_mutex_lock(&mutex);
	
	CPU_ZERO(&set);
	CPU_SET(0, &set);  
	param.__sched_priority = 1;
	
	memset(path, '\0', sizeof(path));
	snprintf(path, sizeof(path), "/var/%d_tid_cache", (int)getpid());
	if ((dir = opendir(path)) == NULL)
		goto bad_out;
	while ((entry = readdir(dir))) {
		if (entry->d_type == DT_REG) {
			/* all regular file name are pthread_t in string form */
			pt = (pthread_t) atol(entry->d_name); 
			tid = pthread_t_to_tid(pt);
			if (tid == -1)
				goto bad_out;
			/* let all threads within the same process run on cpu 0 */	
    		if (sched_setaffinity(tid, sizeof(set), &set) != 0)
				goto bad_out;
			/* change threads policy and priority. need root privilege */
			param.__sched_priority = (pt == (pthread_t) THREAD_SELF) ?
						 __sched_get_priority_min(SCHED_RR)+1 :
						 __sched_get_priority_min(SCHED_RR);
			if (pthread_setschedparam(pt, SCHED_RR, &param) != 0)
				goto bad_out;
		}
	}

	closedir(dir);
	__pthread_mutex_unlock(&mutex);
	return 0;
	
	bad_out:
		closedir(dir);
		__pthread_mutex_unlock(&mutex);
		return -1;
}

/*
 * Description
 * 		Match pthread_lock(). This API support recursive call. 
 * Parameter
 * 		void
 * Return
 *		0	on success
 *		-1  on error
 */
int pthread_unlock(void)
{
	char path[64] = {'\0'};
	DIR *dir = NULL;
	struct dirent *entry;	
	struct pthread *pd = THREAD_SELF;
	pthread_t pt;
	struct sched_param param;	

	if (pd->pthread_lock_counter > 1) {
		--pd->pthread_lock_counter;
		return 0;
	} else if (pd->pthread_lock_counter == 1) {
		--pd->pthread_lock_counter;
	} else {
		return -1;
	}

	__pthread_mutex_lock(&mutex);
	snprintf(path, sizeof(path), "/var/%d_tid_cache", (int)getpid());
	if ((dir = opendir(path)) == NULL)
		goto bad_out;
		
	while ((entry = readdir(dir))) {
		if (entry->d_type == DT_REG) {
			/* all regular file name are pthread_t in string form */
			pt = (pthread_t) atol(entry->d_name); 
			pd = (struct pthread *) pt;
			param.__sched_priority = pd->old_priority;
			if (pthread_setschedparam(pt, pd->old_schepolicy, &param) != 0)
				goto bad_out;					
		}
	}

	closedir(dir);
	__pthread_mutex_unlock(&mutex);
	return 0;

	bad_out:
		closedir(dir);
		__pthread_mutex_unlock(&mutex);
		return -1;
}

/*
 * Description
 *		Get name of pthread_attr_t
 * Parameter
 *		attr	thread attribute
 *		name 	buffer to save name of pthread_attr_t
 * Return
 *		0		on success
 *		EINVAL  on error
 */
int pthread_attr_getname(const pthread_attr_t *attr, char **name)
{
	struct pthread_attr *iattr;
	char *c;

	if (attr == NULL)
		return EINVAL;
	
	assert (sizeof (*attr) >= sizeof (struct pthread_attr));
	iattr = (struct pthread_attr *) attr;	
	for (c=iattr->name; *c!='\0'; c++)
		*((*name)++) = *c;
	**name = '\0';
	
	return 0;
}

/*
 * Description
 *		Set name of pthread_attr_t	
 * Parameter
 *		attr	thread attribute
 *		name	specified name of pthread_attr_t, if NULL used p<pid> as name.
 * Return
 *		0		on success
 *		EINVAL	on error
 * 
 */
int pthread_attr_setname(pthread_attr_t *attr, char *name)
{
	struct pthread_attr *iattr;

	if (attr == NULL)
		return EINVAL;

	assert (sizeof (*attr) >= sizeof (struct pthread_attr));
	iattr = (struct pthread_attr *) attr;
	memset(iattr->name, '\0', sizeof(iattr->name));

	if (name == NULL)
		snprintf(iattr->name, PTHREAD_ATTR_NAME_SIZE, "p%d", (int) getpid());
	else
		snprintf(iattr->name, PTHREAD_ATTR_NAME_SIZE, "%s", name);

	return 0;
}

/*
 * Description:
 *		Force to cancel specfied thread. Ignore the cancel type and cancel state.
 * Return:
 *		0		on success
 *		ESRCH	specfied thread not found
 */
int pthread_cancelforce(pthread_t thread)
{
	struct pthread *pd;
	pd = (struct pthread *) thread;

	/* Make sure the descriptor is valid. */
	if (INVALID_TD_P (pd))
		return ESRCH; /* Not a valid thread handle. */

	/* force cancel state to PTHREAD_CANCEL_ENABLE and 
	   cancel type to PTHREAD_CANCEL_DEFERRED */
	
    int oldval = THREAD_GETMEM (pd, cancelhandling);
  	while (1) {
    	int newval = oldval & ~CANCELSTATE_BITMASK
					        & ~CANCELTYPE_BITMASK;
      	if (oldval == newval)
			break;

        int curval = THREAD_ATOMIC_CMPXCHG_VAL (pd, cancelhandling, newval, oldval);
      	if (__glibc_likely (curval == oldval)) 
			break;
	
      	/* Prepare for the next round.  */
     	 oldval = curval;
    }	

	pthread_cancel(thread);
	return 0;
}


int pthread_getschedprio(pthread_t thread, int *priority)
{
	struct pthread *pd;
	pd = (struct pthread *) thread;

	/* Make sure the descriptor is valid. */
	if (INVALID_TD_P (pd))
		return ESRCH; /* Not a valid thread handle. */	

	if (priority == NULL)
		return EINVAL;

	struct sched_param param;
	int policy = 0;
	int ret;
	
	ret = pthread_getschedparam(thread, &policy, &param);
	if (ret)
		return ret;
	
	*priority = param.sched_priority;
	return 0;
}

/*
 * Description:
 *		Check whether the specified thread exists or not
 * Return:
 * 		0		existing
 *		-1		not found
 */
int pthread_verifyid(pthread_t thread)
{
	struct pthread *pd;
	pd = (struct pthread *) thread;

	if (INVALID_TD_P (pd))
		return -1; 
	else
		return 0;
}

static void tid_to_comm(int tid, char *comm, unsigned int size)
{
	char path[64] = {'\0'};
	snprintf(path, sizeof(path), "/proc/%d/task/%d/comm", (int) getpid(), tid);
	int fd = open(path, O_RDONLY);
	if (fd == -1) {
		snprintf(comm, size, "?Unknown?");
		return;
	}		
	read_line(fd, comm, size);
	close(fd);
	return;
}

static void printf_stack_info(struct pthread *pd)
{
	char thread_name[64] = {'\0'};
	char *p;
	int used, free;
	int i = 0;
	char *top, *bottom;

	tid_to_comm(pd->tid, thread_name, sizeof(thread_name));
	
	/* we can't access the address below bottom */
	top = (char *) pd->stackblock + pd->stackblock_size;
	bottom = (char *) pd->stackblock + pd->guardsize;

	#if _STACK_GROWS_DOWN
	p = bottom;		
	for (; p<=top; p++)
		if (*p != '\0') break;
	free = --p - bottom;
	used = top - bottom - free;
	#else
	p = top;
	for (; p>=bottom; p--)
		if (*p != '\0') break;
	free = top - (++p);
	used = top - bottom - free;
	#endif

	printf("Task:\t\t\t\t%s\n", thread_name);
	printf("ID:\t\t\t\t%d\n", pd->tid);
	printf("Execution routine:\t\t%p\n", pd->start_routine);
	printf("Stack base address:\t\t%p\n", pd->stackblock);
	printf("Stack total size:\t\t%d bytes\n", pd->stackblock_size - pd->guardsize);
	printf("Stack used size:\t\t%d bytes\n", used);
	printf("Stack free size:\t\t%d bytes\n", free);	
}

/*
 * Description:
 *		Show thread stack info, including thread name, execution routine, tid, 
 *		stack base, stack total size, stack used size, stack free size.
 * Parameter:
 * 		if thread is 0, show all threads exclude main thread stack info within 
 *      the invoker process.
 */
void pthread_showstack(pthread_t thread)
{
	struct pthread *pd;
	char path[64] = {'\0'};
	DIR *dir;
	struct dirent *entry;
		
	if (thread != 0) {
		pd = (struct pthread *) thread;
		if (INVALID_TD_P (pd)) {
			printf("[%s] parameter invalid\n", __FUNCTION__);
			return;
		}		
		printf_stack_info(pd);
	} else if (thread == 0) {
		snprintf(path, sizeof(path), "/var/%d%s", (int) getpid(), VAR_PIDDIR_SUFFIX);
		dir = opendir(path);
		if (dir == NULL) {
			printf("[%s] opendir error, errno=%d\n", __FUNCTION__, errno);
			return;
		}		
		while ((entry = readdir(dir))) {
			if (entry->d_type == DT_REG) {
				/* all regular file name are pthread_t in string form */
				pd = (struct pthread *) (atol(entry->d_name));
				if (INVALID_TD_P (pd))
					continue;
				if (pd->tid == pd->pid)
					continue;
				printf_stack_info(pd);
				printf("\n");
			}
		}
		closedir(dir);
	} else {
		printf("[%s] parameter invalid\n", __FUNCTION__);
	}	
}

/*
 * Description:
 *		Add private variable for the specified thread. pvar is the key
 *		and private variable value is *pvar.
 * Return:
 *		0		on success
 *		-1		number of private variable exceeds limit
 *		EINVAL	pvar error
 *		ESRCH	specfied thread not found
 */
int pthread_addvar(pthread_t thread, int *pvar)
{
	struct pthread *pd;
	struct pri_var *p, *pt;
	unsigned int i;
	
	pd = (struct pthread *) thread;
	if (INVALID_TD_P (pd))
		return ESRCH;	

	if (pvar == NULL)
		return EINVAL;

	lll_lock(pd->pri_var_lock, LLL_PRIVATE);
	if (pd->pri_var == NULL) {
		pd->pri_var = (struct pri_var *) malloc(sizeof(struct pri_var));
		pd->pri_var->key = pvar;
		pd->pri_var->value = *pvar;
		pd->pri_var->next = NULL;
		lll_unlock(pd->pri_var_lock, LLL_PRIVATE);
		return 0;
	}
		
	for (i=0, p=pd->pri_var; p!=NULL; p=p->next, i++) {
		if (p->key == pvar) {
			p->value = *pvar;
			lll_unlock(pd->pri_var_lock, LLL_PRIVATE);
			return 0;
		}
		pt = p;
	}
	if (i == THREAD_PRIVATE_VAR_SIZE) {
		lll_unlock(pd->pri_var_lock, LLL_PRIVATE);
		return -1;
	}
	pt->next = (struct pri_var *) malloc(sizeof(struct pri_var));
	pt->next->key = pvar;
	pt->next->value = *pvar;
	pt->next->next = NULL;
	lll_unlock(pd->pri_var_lock, LLL_PRIVATE);

	return 0;
}

/*
 * Description:
 *		Delete private variable from the specified thread. The deleted variable
 *		match to pvar. It means pvar is the key of thread private variable.
 * Return:
 *		0		on success
 *		EINVAL	pvar is NULL or not found
 *		ESRCH	specfied thread not found 
 */
int pthread_delvar(pthread_t thread, int *pvar)
{
	struct pthread *pd;
	struct pri_var *p, *pt = NULL;
	
	pd = (struct pthread *) thread;
	if (INVALID_TD_P (pd))
		return ESRCH;	

	if (pvar == NULL || pd->pri_var == NULL)
		return EINVAL;

	lll_lock(pd->pri_var_lock, LLL_PRIVATE);
	for (p=pd->pri_var; p!=NULL; p=p->next) {
		if (p->key == pvar) {
			if (pt == NULL) {  /* delete head */
				if (p->next == NULL) 
					pd->pri_var = NULL;
				else
					pd->pri_var = p->next;
			} else {
				if (p->next == NULL)
					pt->next = NULL;
				else
					pt->next = p->next;
			} 
			free(p);
			lll_unlock(pd->pri_var_lock, LLL_PRIVATE);
			return 0;
		}
		pt = p;
	}
	lll_unlock(pd->pri_var_lock, LLL_PRIVATE);
	
	if (pt->next == NULL) /* can't find pvar until the last one */
		return EINVAL;
	else
		return 0;
}

/*
 * Description:
 *		Set private variable value for the specfied thread. pvar is the key
 *		of thread private variable.
 * Return:
 *		0		on success
 *		EINVAL	pvar is NULL or not found
 *		ESRCH	specfied thread not found  
 */
int pthread_setvar(pthread_t thread, int *pvar, int value)
{
	struct pthread *pd;
	struct pri_var *p;
	
	pd = (struct pthread *) thread;
	if (INVALID_TD_P (pd))
		return ESRCH;	

	if (pvar == NULL)
		return EINVAL;

	lll_lock(pd->pri_var_lock, LLL_PRIVATE);
	for (p=pd->pri_var; p!=NULL; p=p->next) {
		if (p->key == pvar) {
			p->value = value;
			lll_unlock(pd->pri_var_lock, LLL_PRIVATE);
			return 0;
		}
	}
	lll_unlock(pd->pri_var_lock, LLL_PRIVATE);
	
	return EINVAL;
}

/*
 * Description:
 *		Get private variable value from the specified thread. pvar is the
 *		key of thread private variable.
 * Return:
 *		0		on success
 *		EINVAL	pvar is NULL or not found
 *		ESRCH	specfied thread not found   
 */
int pthread_getvar(pthread_t thread, int *pvar, int *value)
{
	struct pthread *pd;
	struct pri_var *p;
	
	pd = (struct pthread *) thread;
	if (INVALID_TD_P (pd))
		return ESRCH;	

	if (pvar == NULL)
		return EINVAL;

	lll_lock(pd->pri_var_lock, LLL_PRIVATE);
	for (p=pd->pri_var; p!=NULL; p=p->next) {
		if (p->key == pvar) {
			*value = p->value;
			lll_unlock(pd->pri_var_lock, LLL_PRIVATE);
			return 0;
		}
	}
	lll_unlock(pd->pri_var_lock, LLL_PRIVATE);
	
	return EINVAL;
}
