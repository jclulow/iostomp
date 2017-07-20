
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <thread.h>
#include <unistd.h>
#include <synch.h>
#include <err.h>
#include <sys/debug.h>
#include <sys/list.h>
#include <string.h>
#include <signal.h>

#define	WRITE_SIZE	128	/* KB */

char *g_file;
int g_filesz; /* MB */
list_t g_thrlist;

typedef enum thrstate {
	THRST_PRE_RUN = 1,
	THRST_RUNNING,
	THRST_EXIT
} thrstate_t;

typedef struct {
	thrstate_t tw_state;
	boolean_t tw_runok;
	boolean_t tw_exit;

	thread_t tw_tid;
	char *tw_path;
	int tw_fd;
	size_t tw_randosz;
	char *tw_randobuf;
	char tw_nextchar;
	int tw_randocount;

	mutex_t tw_lock;
	cond_t tw_cond;

	int tw_spins;

	list_node_t tw_link;
} thread_writer_t;

static void
thread_set_state(thread_writer_t *tw, thrstate_t st)
{
	VERIFY(MUTEX_HELD(&tw->tw_lock));

	tw->tw_state = st;
	VERIFY0(cond_broadcast(&tw->tw_cond));
}

static void *
thread_writer(void *arg)
{
	thread_writer_t *tw = arg;

	VERIFY0(mutex_lock(&tw->tw_lock));
	while (!tw->tw_runok && !tw->tw_exit) {
		VERIFY0(cond_wait(&tw->tw_cond, &tw->tw_lock));
	}
	if (tw->tw_exit) {
		goto exit;
	}
	VERIFY(tw->tw_runok);
	thread_set_state(tw, THRST_RUNNING);
	VERIFY0(mutex_unlock(&tw->tw_lock));

	/*
	 * Do some I/O work!
	 */
	for (;;) {
		VERIFY0(mutex_lock(&tw->tw_lock));
		if (tw->tw_exit) {
			goto exit;
		}
		tw->tw_spins++;
		VERIFY0(mutex_unlock(&tw->tw_lock));

		/*
		 * Populate the randobuffer.
		 */
		tw->tw_nextchar = (tw->tw_nextchar + (time(NULL) % 17)) % 26 + 'A';
		for (size_t off = 0; off < tw->tw_randosz; off++) {
			if (++tw->tw_nextchar == 'Z') {
				tw->tw_nextchar = 'B';
			}

			tw->tw_randobuf[off] = tw->tw_nextchar;
		}

		if (lseek(tw->tw_fd, 0, SEEK_SET) == -1) {
			err(1, "lseek 0");
		}

		/*
		 * Write the randobuffer to the file randocount times.
		 */
		for (int c = 0; c < tw->tw_randocount; c++) {
			ssize_t act;
			if ((act = write(tw->tw_fd, tw->tw_randobuf,
			    tw->tw_randosz)) < 0) {
				warn("[%d] pwrite error", tw->tw_tid);
			} else if ((size_t)act != tw->tw_randosz) {
				warn("[%d] pwrite short %u", tw->tw_tid, act);
			}
		}

		/*
		 * Sync out to disk.
		 */
		if (fsync(tw->tw_fd) != 0) {
			warn("[%d] fsync error", tw->tw_tid);
		}
	}

	abort();

exit:
	thread_set_state(tw, THRST_EXIT);
	VERIFY0(mutex_unlock(&tw->tw_lock));
	return (NULL);
}

void
init_thread(thread_writer_t **twp, const char *path, size_t randosz, int count)
{
	thread_writer_t *tw;
	sigset_t save;
	sigset_t full;

	VERIFY0(sigfillset(&full));
	VERIFY0(thr_sigsetmask(SIG_BLOCK, &full, &save) != 0);

	if ((tw = calloc(1, sizeof (*tw))) == NULL) {
		err(1, "calloc");
	}

	if ((tw->tw_randobuf = malloc(randosz)) == NULL) {
		err(1, "malloc randobuf");
	}
	tw->tw_randosz = randosz;
	tw->tw_nextchar = 'Z';
	tw->tw_randocount = count;

	if ((tw->tw_path = strdup(path)) == NULL){
		err(1, "strdup");
	}
	if ((tw->tw_fd = open(path, O_RDWR | O_CREAT, 0755)) < 0) {
		err(1, "open \"%s\"", path);
	}

	VERIFY0(mutex_init(&tw->tw_lock, USYNC_THREAD | LOCK_ERRORCHECK,
	    NULL));
	VERIFY0(cond_init(&tw->tw_cond, USYNC_THREAD, NULL));

	tw->tw_state = THRST_PRE_RUN;
	list_insert_tail(&g_thrlist, tw);

	if (thr_create(NULL, 0, thread_writer, tw, 0, &tw->tw_tid) != 0) {
		err(1, "thr_create");
	}

	*twp = tw;
	VERIFY0(thr_sigsetmask(SIG_SETMASK, &save, NULL));
}

void
start_thread(thread_writer_t *tw)
{
	VERIFY0(mutex_lock(&tw->tw_lock));
	tw->tw_runok = B_TRUE;
	VERIFY0(cond_broadcast(&tw->tw_cond));
	VERIFY0(mutex_unlock(&tw->tw_lock));
}

void
stop_thread(thread_writer_t *tw)
{
	VERIFY0(mutex_lock(&tw->tw_lock));
	tw->tw_exit = B_TRUE;
	VERIFY0(cond_broadcast(&tw->tw_cond));
	VERIFY0(mutex_unlock(&tw->tw_lock));
}

void
destroy_thread(thread_writer_t *tw)
{
	if (tw == NULL)
		return;

	VERIFY0(mutex_lock(&tw->tw_lock));
	while (tw->tw_state != THRST_EXIT) {
		tw->tw_exit = B_TRUE;
		VERIFY0(cond_broadcast(&tw->tw_cond));
		VERIFY0(cond_wait(&tw->tw_cond, &tw->tw_lock));
	}
	VERIFY0(mutex_unlock(&tw->tw_lock));

	VERIFY0(close(tw->tw_fd));
	free(tw->tw_randobuf);
	free(tw->tw_path);

	VERIFY0(cond_destroy(&tw->tw_cond));
	VERIFY0(mutex_destroy(&tw->tw_lock));

	free(tw);
}

void
ensure_dir(const char *path)
{
	struct stat st;

	if (stat(path, &st) != 0) {
		err(1, "ensure_dir: stat: %s", path);
	}

	if (!S_ISDIR(st.st_mode)) {
		errx(1, "ensure_dir: is not directory: %s", path);
	}
}

int
count_spins(void)
{
	int spins = 0;

	for (thread_writer_t *tw = list_head(&g_thrlist); tw != NULL;
	    tw = list_next(&g_thrlist, tw)) {
		VERIFY0(mutex_lock(&tw->tw_lock));
		spins += tw->tw_spins;
		tw->tw_spins = 0;
		VERIFY0(mutex_unlock(&tw->tw_lock));
	}

	return (spins);
}

volatile int g_interrupt = 0;

void
handle_sigint(int sig, siginfo_t *sip, void *ctx)
{
	if (g_interrupt == 1)
		_exit(1);
	g_interrupt = 1;
}

int
main(int argc, char *argv[])
{
	int count = 0;
	struct sigaction sa;
	int iocnt;
	int thrs;
#ifdef	LIMIT_SECS
	hrtime_t start = gethrtime();
#endif

	if (argc != 4) {
		fprintf(stderr, "(iostomp with write size %dKB)\n",
		    WRITE_SIZE);
		fprintf(stderr, "usage: %s <dir> <thrs> <iocnt>\n", argv[0]);
		return (1);
	}
	if ((thrs = atoi(argv[2])) < 1 || thrs > 10000) {
		fprintf(stderr, "thrs %d invalid\n", thrs);
		return (1);
	}
	if ((iocnt = atoi(argv[3])) < 1) {
		fprintf(stderr, "iosize %d invalid\n", iocnt);
		return (1);
	}
	ensure_dir(argv[1]);

	list_create(&g_thrlist, sizeof (thread_writer_t),
	    offsetof(thread_writer_t, tw_link));

	size_t writesize = WRITE_SIZE * 1024;

	fprintf(stderr, "allocating threads...\n");
	while (count++ < thrs) {
		thread_writer_t *tw;
		char *apth;

		if (asprintf(&apth, "%s/iostomp.%06d", argv[1], count) < 0) {
			err(1, "asprintf");
		}

		init_thread(&tw, apth, writesize, iocnt);

		free(apth);
	}

	fprintf(stderr, "starting threads...\n");
	for (thread_writer_t *tw = list_head(&g_thrlist); tw != NULL;
	    tw = list_next(&g_thrlist, tw)) {
		start_thread(tw);
	}

	memset(&sa, 0, sizeof (sa));
	sa.sa_flags = SA_SIGINFO;
	sa.sa_sigaction = handle_sigint;
	VERIFY0(sigaction(SIGINT, &sa, NULL));

	fprintf(stderr, "running until SIGINT.\n");
	while (g_interrupt == 0) {
#ifdef	LIMIT_SECS
		int secs = (gethrtime() - start) / 1000000000;

		if (secs > 25) {
			fprintf(stderr, "25 second load time expired\n");
			break;
		}
#endif

		sleep(1);

		int spins = count_spins();
		fprintf(stderr, "[%ld] spins: %d (%lu MB of I/O)\n",
		    time(NULL), spins, (spins * writesize * iocnt) / 1024 / 1024);
	}

	fprintf(stderr, "stopping threads...\n");
	for (thread_writer_t *tw = list_head(&g_thrlist); tw != NULL;
	    tw = list_next(&g_thrlist, tw)) {
		stop_thread(tw);
	}
	fprintf(stderr, "destroying threads...\n");
	while (!list_is_empty(&g_thrlist)) {
		destroy_thread(list_remove_head(&g_thrlist));
	}

	return (0);
}
