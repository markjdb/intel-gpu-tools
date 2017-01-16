/*
 * Copyright © 2007, 2011, 2013, 2014 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Eric Anholt <eric@anholt.net>
 *    Daniel Vetter <daniel.vetter@ffwll.ch>
 *
 */

#ifndef ANDROID
#define _GNU_SOURCE
#else
#include <libgen.h>
#endif
#include <stdio.h>
#include <assert.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <string.h>
#include <sys/mman.h>
#include <signal.h>
#include <pciaccess.h>
#include <getopt.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#include <pthread.h>
#include <sys/utsname.h>
#include <termios.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#include <limits.h>
#include <locale.h>
#include <uwildmat/uwildmat.h>

#include "drmtest.h"
#include "intel_chipset.h"
#include "intel_io.h"
#include "igt_debugfs.h"
#include "version.h"
#include "config.h"

#include "igt_core.h"
#include "igt_aux.h"
#include "igt_sysfs.h"

#ifdef HAVE_LIBGEN_H
#include <libgen.h>   /* for basename() on Solaris */
#endif

/**
 * SECTION:igt_core
 * @short_description: Core i-g-t testing support
 * @title: Core
 * @include: igt.h
 *
 * This library implements the core of the i-g-t test support infrastructure.
 * Main features are the subtest enumeration, cmdline option parsing helpers for
 * subtest handling and various helpers to structure testcases with subtests and
 * handle subtest test results.
 *
 * Auxiliary code provides exit handlers, support for forked processes with test
 * result propagation. Other generally useful functionality includes optional
 * structure logging infrastructure and some support code for running reduced
 * test set on in simulated hardware environments.
 *
 * When writing tests with subtests it is extremely important that nothing
 * interferes with the subtest enumeration. In i-g-t subtests are enumerated at
 * runtime, which allows powerful testcase enumeration. But it makes subtest
 * enumeration a bit more tricky since the test code needs to be careful to
 * never run any code which might fail (like trying to do privileged operations
 * or opening device driver nodes).
 *
 * To allow this i-g-t provides #igt_fixture code blocks for setup code outside
 * of subtests and automatically skips the subtest code blocks themselves. For
 * special cases igt_only_list_subtests() is also provided. For setup code only
 * shared by a group of subtest encapsulate the #igt_fixture block and all the
 * subtestest in a #igt_subtest_group block.
 *
 * # Magic Control Blocks
 *
 * i-g-t makes heavy use of C macros which serve as magic control blocks. They
 * work fairly well and transparently but since C doesn't have full-blown
 * closures there are caveats:
 *
 * - Asynchronous blocks which are used to spawn children internally use fork().
 *   Which means that nonsensical control flow like jumping out of the control
 *   block is possible, but it will badly confuse the i-g-t library code. And of
 *   course all caveats of a real fork() call apply, namely that file
 *   descriptors are copied, but still point at the original file. This will
 *   terminally upset the libdrm buffer manager if both parent and child keep on
 *   using the same open instance of the drm device. Usually everything related
 *   to interacting with the kernel driver must be reinitialized to avoid such
 *   issues.
 *
 * - Code blocks with magic control flow are implemented with setjmp() and
 *   longjmp(). This applies to #igt_fixture and #igt_subtest blocks and all the
 *   three variants to finish test: igt_success(), igt_skip() and igt_fail().
 *   Mostly this is of no concern, except when such a control block changes
 *   stack variables defined in the same function as the control block resides.
 *   Any store/load behaviour after a longjmp() is ill-defined for these
 *   variables. Avoid such code.
 *
 *   Quoting the man page for longjmp():
 *
 *   "The values of automatic variables are unspecified after a call to
 *   longjmp() if they meet all the following criteria:"
 *    - "they are local to the function that made the corresponding setjmp() call;
 *    - "their values are changed between the calls to setjmp() and longjmp(); and
 *    - "they are not declared as volatile."
 *
 * # Best Practices for Test Helper Libraries Design
 *
 * Kernel tests itself tend to have fairly complex logic already. It is
 * therefore paramount that helper code, both in libraries and test-private
 * functions, add as little boilerplate code to the main test logic as possible.
 * But then dense code is hard to understand without constantly consulting
 * the documentation and implementation of all the helper functions if it
 * doesn't follow some clear patterns. Hence follow these established best
 * practices:
 *
 * - Make extensive use of the implicit control flow afforded by igt_skip(),
 *   igt_fail and igt_success(). When dealing with optional kernel features
 *   combine igt_skip() with igt_fail() to skip when the kernel support isn't
 *   available but fail when anything else goes awry. void should be the most
 *   common return type in all your functions, except object constructors of
 *   course.
 *
 * - The main test logic should have no explicit control flow for failure
 *   conditions, but instead such assumptions should be written in a declarative
 *   style.  Use one of the many macros which encapsulate i-g-t's implicit
 *   control flow.  Pick the most suitable one to have as much debug output as
 *   possible without polluting the code unnecessarily. For example
 *   igt_assert_cmpint() for comparing integers or do_ioctl() for running ioctls
 *   and checking their results.  Feel free to add new ones to the library or
 *   wrap up a set of checks into a private function to further condense your
 *   test logic.
 *
 * - When adding a new feature test function which uses igt_skip() internally,
 *   use the <prefix>_require_<feature_name> naming scheme. When you
 *   instead add a feature test function which returns a boolean, because your
 *   main test logic must take different actions depending upon the feature's
 *   availability, then instead use the <prefix>_has_<feature_name>.
 *
 * - As already mentioned eschew explicit error handling logic as much as
 *   possible. If your test absolutely has to handle the error of some function
 *   the customary naming pattern is to prefix those variants with __. Try to
 *   restrict explicit error handling to leaf functions. For the main test flow
 *   simply pass the expected error condition down into your helper code, which
 *   results in tidy and declarative test logic.
 *
 * - Make your library functions as simple to use as possible. Automatically
 *   register cleanup handlers through igt_install_exit_handler(). Reduce the
 *   amount of setup boilerplate needed by using implicit singletons and lazy
 *   structure initialization and similar design patterns.
 *
 * - Don't shy away from refactoring common code, even when there are just 2-3
 *   users and even if it's not a net reduction in code. As long as it helps to
 *   remove boilerplate and makes the code more declarative the resulting
 *   clearer test flow is worth it. All i-g-t library code has been organically
 *   extracted from testcases in this fashion.
 *
 * - For general coding style issues please follow the kernel's rules laid out
 *   in
 *   [CodingStyle](https://www.kernel.org/doc/Documentation/CodingStyle).
 *
 * # Interface with Testrunners
 *
 * i-g-t testcase are all executables which should be run as root on an
 * otherwise completely idle system. The test status is reflected in the
 * exitcode. #IGT_EXIT_SUCCESS means "success", #IGT_EXIT_SKIP "skip",
 * #IGT_EXIT_TIMEOUT that some operation "timed out".  All other exit codes
 * encode a failed test result, including any abnormal termination of the test
 * (e.g. by SIGKILL).
 *
 * On top of that tests may report unexpected results and minor issues to
 * stderr. If stderr is non-empty the test result should be treated as "warn".
 *
 * The test lists are generated at build time. Simple testcases are listed in
 * tests/single-tests.txt and tests with subtests are listed in
 * tests/multi-tests.txt. When running tests with subtest from a test runner it
 * is recommend to run each subtest individually, since otherwise the return
 * code will only reflect the overall result.
 *
 * To do that obtain the lists of subtests with "--list-subtests", which can be
 * run as non-root and doesn't require the i915 driver to be loaded (or any
 * intel gpu to be present). Then individual subtests can be run with
 * "--run-subtest". Usage help for tests with subtests can be obtained with the
 * "--help" command line option.
 *
 * A wildcard expression can be given to --run-subtest to specify a subset of
 * subtests to run. See https://tools.ietf.org/html/rfc3977#section-4 for a
 * description of allowed wildcard expressions.
 * Some examples of allowed wildcard expressions are:
 *
 * - '*basic*' match any subtest containing basic
 * - 'basic-???' match any subtest named basic- with 3 characters after -
 * - 'basic-[0-9]' match any subtest named basic- with a single number after -
 * - 'basic-[^0-9]' match any subtest named basic- with a single non numerical character after -
 * - 'basic*,advanced*' match any subtest starting basic or advanced
 * - '*,!basic*' match any subtest not starting basic
 * - 'basic*,!basic-render*' match any subtest starting basic but not starting basic-render
 */

static unsigned int exit_handler_count;
const char *igt_interactive_debug;

/* subtests helpers */
static bool list_subtests = false;
static char *run_single_subtest = NULL;
static bool run_single_subtest_found = false;
static const char *in_subtest = NULL;
static struct timespec subtest_time;
static clockid_t igt_clock = (clockid_t)-1;
static bool in_fixture = false;
static bool test_with_subtests = false;
static bool in_atexit_handler = false;
static enum {
	CONT = 0, SKIP, FAIL
} skip_subtests_henceforth = CONT;

bool __igt_plain_output = false;

/* fork support state */
pid_t *test_children;
int num_test_children;
int test_children_sz;
bool test_child;

enum {
 OPT_LIST_SUBTESTS,
 OPT_RUN_SUBTEST,
 OPT_DESCRIPTION,
 OPT_DEBUG,
 OPT_INTERACTIVE_DEBUG,
 OPT_HELP = 'h'
};

static int igt_exitcode = IGT_EXIT_SUCCESS;
static const char *command_str;

static char* igt_log_domain_filter;
static struct {
	char *entries[256];
	uint8_t start, end;
} log_buffer;
static pthread_mutex_t log_buffer_mutex = PTHREAD_MUTEX_INITIALIZER;

const char *igt_test_name(void)
{
	return command_str;
}

static void _igt_log_buffer_append(char *line)
{
	pthread_mutex_lock(&log_buffer_mutex);

	free(log_buffer.entries[log_buffer.end]);
	log_buffer.entries[log_buffer.end] = line;
	log_buffer.end++;
	if (log_buffer.end == log_buffer.start)
		log_buffer.start++;

	pthread_mutex_unlock(&log_buffer_mutex);
}

static void _igt_log_buffer_reset(void)
{
	pthread_mutex_lock(&log_buffer_mutex);

	log_buffer.start = log_buffer.end = 0;

	pthread_mutex_unlock(&log_buffer_mutex);
}

static void _igt_log_buffer_dump(void)
{
	uint8_t i;

	if (in_subtest)
		fprintf(stderr, "Subtest %s failed.\n", in_subtest);
	else
		fprintf(stderr, "Test %s failed.\n", command_str);

	if (log_buffer.start == log_buffer.end) {
		fprintf(stderr, "No log.\n");
		return;
	}

	pthread_mutex_lock(&log_buffer_mutex);
	fprintf(stderr, "**** DEBUG ****\n");

	i = log_buffer.start;
	do {
		char *last_line = log_buffer.entries[i];
		fprintf(stderr, "%s", last_line);
		i++;
	} while (i != log_buffer.start && i != log_buffer.end);

	/* reset the buffer */
	log_buffer.start = log_buffer.end = 0;

	fprintf(stderr, "****  END  ****\n");
	pthread_mutex_unlock(&log_buffer_mutex);
}

__attribute__((format(printf, 1, 2)))
static void kmsg(const char *format, ...)
#define KERN_EMER	"<0>"
#define KERN_ALERT	"<1>"
#define KERN_CRIT	"<2>"
#define KERN_ERR	"<3>"
#define KERN_WARNING	"<4>"
#define KERN_NOTICE	"<5>"
#define KERN_INFO	"<6>"
#define KERN_DEBUG	"<7>"
{
	va_list ap;
	FILE *file;

	file = fopen("/dev/kmsg", "w");
	if (file == NULL)
		return;

	va_start(ap, format);
	vfprintf(file, format, ap);
	va_end(ap);

	fclose(file);
}

#define time_valid(ts) ((ts)->tv_sec || (ts)->tv_nsec)

static double
time_elapsed(struct timespec *then,
	     struct timespec* now)
{
	double elapsed = -1.;

	if (time_valid(then) && time_valid(now)) {
		elapsed = now->tv_sec - then->tv_sec;
		elapsed += (now->tv_nsec - then->tv_nsec) * 1e-9;
	}

	return elapsed;
}

static int gettime(struct timespec *ts)
{
	memset(ts, 0, sizeof(*ts));
	errno = 0;

	/* Stay on the same clock for consistency. */
	if (igt_clock != (clockid_t)-1) {
		if (clock_gettime(igt_clock, ts))
			goto error;
		return 0;
	}

#ifdef CLOCK_MONOTONIC_RAW
	if (!clock_gettime(igt_clock = CLOCK_MONOTONIC_RAW, ts))
		return 0;
#endif
#ifdef CLOCK_MONOTONIC_COARSE
	if (!clock_gettime(igt_clock = CLOCK_MONOTONIC_COARSE, ts))
		return 0;
#endif
	if (!clock_gettime(igt_clock = CLOCK_MONOTONIC, ts))
		return 0;
error:
	igt_warn("Could not read monotonic time: %s\n",
		 strerror(errno));

	return -errno;
}

uint64_t igt_nsec_elapsed(struct timespec *start)
{
	struct timespec now;

	gettime(&now);
	if ((start->tv_sec | start->tv_nsec) == 0) {
		*start = now;
		return 0;
	}

	return ((now.tv_nsec - start->tv_nsec) +
		(uint64_t)NSEC_PER_SEC*(now.tv_sec - start->tv_sec));
}

bool __igt_fixture(void)
{
	assert(!in_fixture);

	if (igt_only_list_subtests())
		return false;

	if (skip_subtests_henceforth)
		return false;

	in_fixture = true;
	return true;
}

void __igt_fixture_complete(void)
{
	assert(in_fixture);

	in_fixture = false;
}

void __igt_fixture_end(void)
{
	assert(in_fixture);

	in_fixture = false;
	siglongjmp(igt_subtest_jmpbuf, 1);
}

/*
 * Some of the IGT tests put quite a lot of pressure on memory and when
 * running on Android they are sometimes killed by the Android low memory killer.
 * This seems to be due to some incompatibility between the kswapd free memory
 * targets and the way the lowmemorykiller assesses free memory.
 * The low memory killer really isn't usefull in this context and has no
 * interaction with the gpu driver that we are testing, so the following
 * function is used to disable it by modifying one of its module parameters.
 * We still have the normal linux oom killer to protect the kernel.
 * Apparently it is also possible for the lowmemorykiller to get included
 * in some linux distributions; so rather than check for Android we directly
 * check for the existence of the module parameter we want to adjust.
 *
 * In future, if we can get the lowmemorykiller to play nicely then we can
 * remove this hack.
 */
static void low_mem_killer_disable(bool disable)
{
	static const char* adj_fname="/sys/module/lowmemorykiller/parameters/adj";
	static const char no_lowmem_killer[] = "9999";
	int fd;
	struct stat buf;
	/* The following must persist across invocations */
	static char prev_adj_scores[256];
	static int adj_scores_len = 0;
	static bool is_disabled = false;

	/* capture the permissions bits for the lowmemkiller adj pseudo-file.
	 * Bail out if the stat fails; it probably means that there is no
	 * lowmemorykiller, but in any case we're doomed. */
	if (stat(adj_fname, &buf)) {
		igt_assert(errno == ENOENT);
		return;
	}

	/* make sure the file can be read/written - by default it is write-only */
	chmod(adj_fname, S_IRUSR | S_IWUSR);

	if (disable && !is_disabled) {
		/* read the current oom adj parameters for lowmemorykiller */
		fd = open(adj_fname, O_RDWR);
		igt_assert(fd != -1);
		adj_scores_len = read(fd, (void*)prev_adj_scores, 255);
		igt_assert(adj_scores_len > 0);

		/* writing 9999 to this module parameter effectively diables the
		 * low memory killer. This is not a real file, so we dont need to
		 * seek to the start or truncate it */
		igt_assert_eq(write(fd, no_lowmem_killer, sizeof(no_lowmem_killer)),
			      sizeof(no_lowmem_killer));
		close(fd);
		is_disabled = true;
	} else if (is_disabled) {
		/* just re-enstate the original settings */
		fd = open(adj_fname, O_WRONLY);
		igt_assert(fd != -1);
		igt_assert_eq(write(fd, prev_adj_scores, adj_scores_len),
			      adj_scores_len);
		close(fd);
		is_disabled = false;
	}

	/* re-enstate the file permissions */
	chmod(adj_fname, buf.st_mode);
}

bool igt_exit_called;
static void common_exit_handler(int sig)
{
	if (!igt_only_list_subtests()) {
		low_mem_killer_disable(false);
		kick_fbcon(true);
	}

	/* When not killed by a signal check that igt_exit() has been properly
	 * called. */
	assert(sig != 0 || igt_exit_called);
}

static void print_test_description(void)
{
	if (&__igt_test_description)
		printf("%s\n", __igt_test_description);
}

static void print_version(void)
{
	struct utsname uts;

	if (list_subtests)
		return;

	uname(&uts);

	fprintf(stdout, "IGT-Version: %s-%s (%s) (%s: %s %s)\n", PACKAGE_VERSION,
		IGT_GIT_SHA1, TARGET_CPU_PLATFORM,
		uts.sysname, uts.release, uts.machine);
}

static void print_usage(const char *help_str, bool output_on_stderr)
{
	FILE *f = output_on_stderr ? stderr : stdout;

	fprintf(f, "Usage: %s [OPTIONS]\n", command_str);
	fprintf(f, "  --list-subtests\n"
		   "  --run-subtest <pattern>\n"
		   "  --debug[=log-domain]\n"
		   "  --interactive-debug[=domain]\n"
		   "  --help-description\n"
		   "  --help\n");
	if (help_str)
		fprintf(f, "%s\n", help_str);
}


static void oom_adjust_for_doom(void)
{
#ifndef __FreeBSD__
	int fd;
	const char always_kill[] = "1000";

	fd = open("/proc/self/oom_score_adj", O_WRONLY);
	igt_assert(fd != -1);
	igt_assert(write(fd, always_kill, sizeof(always_kill)) == sizeof(always_kill));
	close(fd);
#endif
}

static int common_init(int *argc, char **argv,
		       const char *extra_short_opts,
		       const struct option *extra_long_opts,
		       const char *help_str,
		       igt_opt_handler_t extra_opt_handler,
		       void *handler_data)
{
	int c, option_index = 0, i, x;
	static struct option long_options[] = {
		{"list-subtests", 0, 0, OPT_LIST_SUBTESTS},
		{"run-subtest", 1, 0, OPT_RUN_SUBTEST},
		{"help-description", 0, 0, OPT_DESCRIPTION},
		{"debug", optional_argument, 0, OPT_DEBUG},
		{"interactive-debug", optional_argument, 0, OPT_INTERACTIVE_DEBUG},
		{"help", 0, 0, OPT_HELP},
		{0, 0, 0, 0}
	};
	char *short_opts;
	const char *std_short_opts = "h";
	struct option *combined_opts;
	int extra_opt_count;
	int all_opt_count;
	int ret = 0;
	const char *env;

	if (!isatty(STDOUT_FILENO) || getenv("IGT_PLAIN_OUTPUT"))
		__igt_plain_output = true;

	if (!__igt_plain_output)
		setlocale(LC_ALL, "");

	env = getenv("IGT_LOG_LEVEL");
	if (env) {
		if (strcmp(env, "debug") == 0)
			igt_log_level = IGT_LOG_DEBUG;
		else if (strcmp(env, "info") == 0)
			igt_log_level = IGT_LOG_INFO;
		else if (strcmp(env, "warn") == 0)
			igt_log_level = IGT_LOG_WARN;
		else if (strcmp(env, "none") == 0)
			igt_log_level = IGT_LOG_NONE;
	}

	command_str = argv[0];
	if (strrchr(command_str, '/'))
		command_str = strrchr(command_str, '/') + 1;

	/* First calculate space for all passed-in extra long options */
	all_opt_count = 0;
	while (extra_long_opts && extra_long_opts[all_opt_count].name) {

		/* check for conflicts with standard long option values */
		for (i = 0; long_options[i].name; i++)
			if (extra_long_opts[all_opt_count].val == long_options[i].val)
				igt_warn("Conflicting long option values between --%s and --%s\n",
					 extra_long_opts[all_opt_count].name,
					 long_options[i].name);

		/* check for conflicts with short options */
		if (extra_long_opts[all_opt_count].val != ':'
		    && strchr(std_short_opts, extra_long_opts[all_opt_count].val)) {
			igt_warn("Conflicting long and short option values between --%s and -%s\n",
				 extra_long_opts[all_opt_count].name,
				 long_options[i].name);
		}


		all_opt_count++;
	}
	extra_opt_count = all_opt_count;

	/* check for conflicts in extra short options*/
	for (i = 0; extra_short_opts && extra_short_opts[i]; i++) {

		if (extra_short_opts[i] == ':')
			continue;

		/* check for conflicts with standard short options */
		if (strchr(std_short_opts, extra_short_opts[i]))
			igt_warn("Conflicting short option: -%c\n", std_short_opts[i]);

		/* check for conflicts with standard long option values */
		for (x = 0; long_options[x].name; x++)
			if (long_options[x].val == extra_short_opts[i])
				igt_warn("Conflicting short option and long option value: --%s and -%c\n",
					 long_options[x].name, extra_short_opts[i]);
	}

	all_opt_count += ARRAY_SIZE(long_options);

	combined_opts = malloc(all_opt_count * sizeof(*combined_opts));
	memcpy(combined_opts, extra_long_opts,
	       extra_opt_count * sizeof(*combined_opts));

	/* Copy the subtest long options (and the final NULL entry) */
	memcpy(&combined_opts[extra_opt_count], long_options,
		ARRAY_SIZE(long_options) * sizeof(*combined_opts));

	ret = asprintf(&short_opts, "%s%s",
		       extra_short_opts ? extra_short_opts : "",
		       std_short_opts);
	assert(ret >= 0);

	while ((c = getopt_long(*argc, argv, short_opts, combined_opts,
			       &option_index)) != -1) {
		switch(c) {
		case OPT_INTERACTIVE_DEBUG:
			if (optarg && strlen(optarg) > 0)
				igt_interactive_debug = strdup(optarg);
			else
				igt_interactive_debug = "all";
			break;
		case OPT_DEBUG:
			igt_log_level = IGT_LOG_DEBUG;
			if (optarg && strlen(optarg) > 0)
				igt_log_domain_filter = strdup(optarg);
			break;
		case OPT_LIST_SUBTESTS:
			if (!run_single_subtest)
				list_subtests = true;
			break;
		case OPT_RUN_SUBTEST:
			if (!list_subtests)
				run_single_subtest = strdup(optarg);
			break;
		case OPT_DESCRIPTION:
			print_test_description();
			ret = -1;
			goto out;
		case OPT_HELP:
			print_usage(help_str, false);
			ret = -1;
			goto out;
		case '?':
			print_usage(help_str, true);
			ret = -2;
			goto out;
		default:
			ret = extra_opt_handler(c, option_index, handler_data);
			if (ret)
				goto out;
		}
	}

out:
	free(short_opts);
	free(combined_opts);

	/* exit immediately if this test has no subtests and a subtest or the
	 * list of subtests has been requested */
	if (!test_with_subtests) {
		if (run_single_subtest) {
			igt_warn("Unknown subtest: %s\n", run_single_subtest);
			exit(IGT_EXIT_INVALID);
		}
		if (list_subtests)
			exit(IGT_EXIT_INVALID);
	}

	if (ret < 0)
		/* exit with no error for -h/--help */
		exit(ret == -1 ? 0 : IGT_EXIT_INVALID);

	if (!list_subtests) {
		kick_fbcon(false);
		kmsg(KERN_INFO "[IGT] %s: executing\n", command_str);
		print_version();

		oom_adjust_for_doom();
		low_mem_killer_disable(true);
	}

	/* install exit handler, to ensure we clean up */
	igt_install_exit_handler(common_exit_handler);

	if (!test_with_subtests)
		gettime(&subtest_time);

	for (i = 0; (optind + i) < *argc; i++)
		argv[i + 1] = argv[optind + i];

	*argc = *argc - optind + 1;

	return ret;
}


/**
 * igt_subtest_init_parse_opts:
 * @argc: argc from the test's main()
 * @argv: argv from the test's main()
 * @extra_short_opts: getopt_long() compliant list with additional short options
 * @extra_long_opts: getopt_long() compliant list with additional long options
 * @help_str: help string for the additional options
 * @extra_opt_handler: handler for the additional options
 * @handler_data: user data given to @extra_opt_handler when invoked
 *
 * This function handles the subtest related command line options and allows an
 * arbitrary set of additional options. This is useful for tests which have
 * additional knobs to tune when run manually like the number of rounds execute
 * or the size of the allocated buffer objects.
 *
 * Tests without special needs should just use igt_subtest_init() or use
 * #igt_main directly instead of their own main() function.
 *
 * Returns: Forwards any option parsing errors from getopt_long.
 */
int igt_subtest_init_parse_opts(int *argc, char **argv,
				const char *extra_short_opts,
				const struct option *extra_long_opts,
				const char *help_str,
				igt_opt_handler_t extra_opt_handler,
				void *handler_data)
{
	int ret;

	test_with_subtests = true;
	ret = common_init(argc, argv, extra_short_opts, extra_long_opts,
			  help_str, extra_opt_handler, handler_data);

	return ret;
}

enum igt_log_level igt_log_level = IGT_LOG_INFO;

/**
 * igt_simple_init_parse_opts:
 * @argc: argc from the test's main()
 * @argv: argv from the test's main()
 * @extra_short_opts: getopt_long() compliant list with additional short options
 * @extra_long_opts: getopt_long() compliant list with additional long options
 * @help_str: help string for the additional options
 * @extra_opt_handler: handler for the additional options
 * @handler_data: user data given to @extra_opt_handler when invoked
 *
 * This initializes a simple test without any support for subtests and allows
 * an arbitrary set of additional options.
 */
void igt_simple_init_parse_opts(int *argc, char **argv,
				const char *extra_short_opts,
				const struct option *extra_long_opts,
				const char *help_str,
				igt_opt_handler_t extra_opt_handler,
				void *handler_data)
{
	common_init(argc, argv, extra_short_opts, extra_long_opts, help_str,
		    extra_opt_handler, handler_data);
}

/*
 * Note: Testcases which use these helpers MUST NOT output anything to stdout
 * outside of places protected by igt_run_subtest checks - the piglit
 * runner adds every line to the subtest list.
 */
bool __igt_run_subtest(const char *subtest_name)
{
	int i;

	assert(!in_subtest);
	assert(!in_fixture);
	assert(test_with_subtests);

	/* check the subtest name only contains a-z, A-Z, 0-9, '-' and '_' */
	for (i = 0; subtest_name[i] != '\0'; i++)
		if (subtest_name[i] != '_' && subtest_name[i] != '-'
		    && !isalnum(subtest_name[i])) {
			igt_critical("Invalid subtest name \"%s\".\n",
				     subtest_name);
			igt_exit();
		}

	if (list_subtests) {
		printf("%s\n", subtest_name);
		return false;
	}

	if (run_single_subtest) {
		if (uwildmat(subtest_name, run_single_subtest) == 0)
			return false;
		else
			run_single_subtest_found = true;
	}

	if (skip_subtests_henceforth) {
		printf("%sSubtest %s: %s%s\n",
		       (!__igt_plain_output) ? "\x1b[1m" : "", subtest_name,
		       skip_subtests_henceforth == SKIP ?
		       "SKIP" : "FAIL", (!__igt_plain_output) ? "\x1b[0m" : "");
		return false;
	}

	kmsg(KERN_INFO "[IGT] %s: starting subtest %s\n", command_str, subtest_name);
	igt_debug("Starting subtest: %s\n", subtest_name);

	_igt_log_buffer_reset();

	gettime(&subtest_time);
	return (in_subtest = subtest_name);
}

/**
 * igt_subtest_name:
 *
 * Returns: The name of the currently executed subtest or NULL if called from
 * outside a subtest block.
 */
const char *igt_subtest_name(void)
{
	return in_subtest;
}

/**
 * igt_only_list_subtests:
 *
 * Returns: Returns true if only subtest should be listed and any setup code
 * must be skipped, false otherwise.
 */
bool igt_only_list_subtests(void)
{
	return list_subtests;
}

void __igt_subtest_group_save(int *save)
{
	*save = skip_subtests_henceforth;
}

void __igt_subtest_group_restore(int save)
{
	skip_subtests_henceforth = save;
}

static bool skipped_one = false;
static bool succeeded_one = false;
static bool failed_one = false;

static void exit_subtest(const char *) __attribute__((noreturn));
static void exit_subtest(const char *result)
{
	struct timespec now;

	gettime(&now);
	printf("%sSubtest %s: %s (%.3fs)%s\n",
	       (!__igt_plain_output) ? "\x1b[1m" : "",
	       in_subtest, result, time_elapsed(&subtest_time, &now),
	       (!__igt_plain_output) ? "\x1b[0m" : "");
	fflush(stdout);

	in_subtest = NULL;
	siglongjmp(igt_subtest_jmpbuf, 1);
}

/**
 * igt_skip:
 * @f: format string
 * @...: optional arguments used in the format string
 *
 * Subtest aware test skipping. The format string is printed to stderr as the
 * reason why the test skipped.
 *
 * For tests with subtests this will either bail out of the current subtest or
 * mark all subsequent subtests as SKIP (presuming some global setup code
 * failed).
 *
 * For normal tests without subtest it will directly exit.
 */
void igt_skip(const char *f, ...)
{
	va_list args;
	skipped_one = true;

	assert(!test_child);

	if (!igt_only_list_subtests()) {
		va_start(args, f);
		vprintf(f, args);
		va_end(args);
	}

	if (in_subtest) {
		exit_subtest("SKIP");
	} else if (test_with_subtests) {
		skip_subtests_henceforth = SKIP;
		assert(in_fixture);
		__igt_fixture_end();
	} else {
		igt_exitcode = IGT_EXIT_SKIP;
		igt_exit();
	}
}

void __igt_skip_check(const char *file, const int line,
		      const char *func, const char *check,
		      const char *f, ...)
{
	va_list args;
	int err = errno;
	char *err_str = NULL;

	if (err)
		igt_assert_neq(asprintf(&err_str, "Last errno: %i, %s\n", err, strerror(err)),
			       -1);

	if (f) {
		static char *buf;

		/* igt_skip never returns, so try to not leak too badly. */
		if (buf)
			free(buf);

		va_start(args, f);
		igt_assert_neq(vasprintf(&buf, f, args), -1);
		va_end(args);

		igt_skip("Test requirement not met in function %s, file %s:%i:\n"
			 "Test requirement: %s\n%s"
			 "%s",
			 func, file, line, check, buf, err_str ?: "");
	} else {
		igt_skip("Test requirement not met in function %s, file %s:%i:\n"
			 "Test requirement: %s\n"
			 "%s",
			 func, file, line, check, err_str ?: "");
	}
}

/**
 * igt_success:
 *
 * Complete a (subtest) as successful
 *
 * This bails out of a subtests and marks it as successful. For global tests it
 * it won't bail out of anything.
 */
void igt_success(void)
{
	succeeded_one = true;
	if (in_subtest)
		exit_subtest("SUCCESS");
}

/**
 * igt_fail:
 * @exitcode: exitcode
 *
 * Fail a testcase. The exitcode is used as the exit code of the test process.
 * It may not be 0 (which indicates success) or 77 (which indicates a skipped
 * test).
 *
 * For tests with subtests this will either bail out of the current subtest or
 * mark all subsequent subtests as FAIL (presuming some global setup code
 * failed).
 *
 * For normal tests without subtest it will directly exit with the given
 * exitcode.
 */
void igt_fail(int exitcode)
{
	assert(exitcode != IGT_EXIT_SUCCESS && exitcode != IGT_EXIT_SKIP);

	igt_debug_wait_for_keypress("failure");

	/* Exit immediately if the test is already exiting and igt_fail is
	 * called. This can happen if an igt_assert fails in an exit handler */
	if (in_atexit_handler)
		_exit(IGT_EXIT_FAILURE);

	if (!failed_one)
		igt_exitcode = exitcode;

	failed_one = true;

	/* Silent exit, parent will do the yelling. */
	if (test_child)
		exit(exitcode);

	_igt_log_buffer_dump();

	if (in_subtest) {
		if (exitcode == IGT_EXIT_TIMEOUT)
			exit_subtest("TIMEOUT");
		else
			exit_subtest("FAIL");
	} else {
		assert(!test_with_subtests || in_fixture);

		if (in_fixture) {
			skip_subtests_henceforth = FAIL;
			__igt_fixture_end();
		}

		igt_exit();
	}
}

static bool run_under_gdb(void)
{
	char buf[1024];

	sprintf(buf, "/proc/%d/exe", getppid());
	return (readlink (buf, buf, sizeof (buf)) != -1 &&
		strncmp(basename(buf), "gdb", 3) == 0);
}

#ifdef HAVE_LIBUNWIND
#define UNW_LOCAL_ONLY
#include <libunwind.h>

static void print_backtrace(void)
{
	unw_cursor_t cursor;
	unw_context_t uc;
	int stack_num = 0;

	printf("Stack trace:\n");

	unw_getcontext(&uc);
	unw_init_local(&cursor, &uc);
	while (unw_step(&cursor) > 0) {
		char name[255];
		unw_word_t off;

		if (unw_get_proc_name(&cursor, name, 255, &off) < 0)
			strcpy(name, "<unknown>");

		printf("  #%d [%s+0x%x]\n", stack_num++, name,
		       (unsigned int) off);
	}
}

static const char hex[] = "0123456789abcdef";

static void
xputch(int c)
{
	write(STDERR_FILENO, (const void *) &c, 1);
}

static int
xpow(int base, int pow)
{
	int i, r = 1;

	for (i = 0; i < pow; i++)
		r *= base;

	return r;
}

static void
printnum(unsigned long long num, unsigned base)
{
	int i = 0;
	unsigned long long __num = num;

	/* determine from where we should start dividing */
	do {
		__num /= base;
		i++;
	} while (__num);

	while (i--)
		xputch(hex[num / xpow(base, i) % base]);
}

static size_t
xstrlcpy(char *dst, const char *src, size_t size)
{
	char *dst_in;

	dst_in = dst;
	if (size > 0) {
		while (--size > 0 && *src != '\0')
			*dst++ = *src++;
		*dst = '\0';
	}

	return dst - dst_in;
}

static void
xprintfmt(const char *fmt, va_list ap)
{
	const char *p;
	int ch, base;
	unsigned long long num;

	while (1) {
		while ((ch = *(const unsigned char *) fmt++) != '%') {
			if (ch == '\0') {
				return;
			}
			xputch(ch);
		}

		ch = *(const unsigned char *) fmt++;
		switch (ch) {
		/* character */
		case 'c':
			xputch(va_arg(ap, int));
			break;
		/* string */
		case 's':
			if ((p = va_arg(ap, char *)) == NULL) {
				p = "(null)";
			}

			for (; (ch = *p++) != '\0';) {
				if (ch < ' ' || ch > '~') {
					xputch('?');
				} else {
					xputch(ch);
				}
			}
			break;
		/* (signed) decimal */
		case 'd':
			num = va_arg(ap, int);
			if ((long long) num < 0) {
				xputch('-');
				num = -(long long) num;
			}
			base = 10;
			goto number;
		/* unsigned decimal */
		case 'u':
			num = va_arg(ap, unsigned int);
			base = 10;
			goto number;
		/* (unsigned) hexadecimal */
		case 'x':
			num = va_arg(ap, unsigned int);
			base = 16;
number:
			printnum(num, base);
			break;

		/* The following are not implemented */

		/* width field */
		case '1': case '2':
		case '3': case '4':
		case '5': case '6':
		case '7': case '8':
		case '9':
		case '.': case '#':
		/* long */
		case 'l':
		/* octal */
		case 'o':
		/* pointer */
		case 'p':
		/* float */
		case 'f':
			abort();
		/* escaped '%' character */
		case '%':
			xputch(ch);
			break;
		/* unrecognized escape sequence - just print it literally */
		default:
			xputch('%');
			for (fmt--; fmt[-1] != '%'; fmt--)
				; /* do nothing */
			break;
		}
	}
}

/* async-safe printf */
static void
xprintf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	xprintfmt(fmt, ap);
	va_end(ap);
}

static void print_backtrace_sig_safe(void)
{
	unw_cursor_t cursor;
	unw_context_t uc;
	int stack_num = 0;

	write(STDERR_FILENO, "Stack trace: \n", 15);

	unw_getcontext(&uc);
	unw_init_local(&cursor, &uc);
	while (unw_step(&cursor) > 0) {
		char name[255];
		unw_word_t off;

		if (unw_get_proc_name(&cursor, name, 255, &off) < 0)
			xstrlcpy(name, "<unknown>", 10);

		xprintf(" #%d [%s+0x%x]\n", stack_num++, name,
				(unsigned int) off);

	}
}
#endif

void __igt_fail_assert(const char *domain, const char *file, const int line,
		       const char *func, const char *assertion,
		       const char *f, ...)
{
	va_list args;
	int err = errno;

	igt_log(domain, IGT_LOG_CRITICAL,
		"Test assertion failure function %s, file %s:%i:\n", func, file,
		line);
	igt_log(domain, IGT_LOG_CRITICAL, "Failed assertion: %s\n", assertion);
	if (err)
		igt_log(domain, IGT_LOG_CRITICAL, "Last errno: %i, %s\n", err,
			strerror(err));

	if (f) {
		va_start(args, f);
		igt_vlog(domain, IGT_LOG_CRITICAL, f, args);
		va_end(args);
	}

#ifdef HAVE_LIBUNWIND
	print_backtrace();
#endif

	if (run_under_gdb())
		abort();
	igt_fail(IGT_EXIT_FAILURE);
}

/**
 * igt_exit:
 *
 * exit() for both types (simple and with subtests) of i-g-t tests.
 *
 * This will exit the test with the right exit code when subtests have been
 * skipped. For normal tests it exits with a successful exit code, presuming
 * everything has worked out. For subtests it also checks that at least one
 * subtest has been run (save when only listing subtests.
 *
 * It is an error to normally exit a test calling igt_exit() - without it the
 * result reporting will be wrong. To avoid such issues it is highly recommended
 * to use #igt_main or #igt_simple_main instead of a hand-rolled main() function.
 */
void igt_exit(void)
{
	igt_exit_called = true;

	if (run_single_subtest && !run_single_subtest_found) {
		igt_warn("Unknown subtest: %s\n", run_single_subtest);
		exit(IGT_EXIT_INVALID);
	}

	if (igt_only_list_subtests())
		exit(IGT_EXIT_SUCCESS);

	/* Calling this without calling one of the above is a failure */
	assert(!test_with_subtests ||
	       skipped_one ||
	       succeeded_one ||
	       failed_one);

	if (test_with_subtests && !failed_one) {
		if (succeeded_one)
			igt_exitcode = IGT_EXIT_SUCCESS;
		else
			igt_exitcode = IGT_EXIT_SKIP;
	}

	if (command_str)
		kmsg(KERN_INFO "[IGT] %s: exiting, ret=%d\n",
		     command_str, igt_exitcode);
	igt_debug("Exiting with status code %d\n", igt_exitcode);

	for (int c = 0; c < num_test_children; c++)
		kill(test_children[c], SIGKILL);

	if (!test_with_subtests) {
		struct timespec now;
		const char *result;

		gettime(&now);

		switch (igt_exitcode) {
			case IGT_EXIT_SUCCESS:
				result = "SUCCESS";
				break;
			case IGT_EXIT_TIMEOUT:
				result = "TIMEOUT";
				break;
			case IGT_EXIT_SKIP:
				result = "SKIP";
				break;
			default:
				result = "FAIL";
		}

		printf("%s (%.3fs)\n",
		       result, time_elapsed(&subtest_time, &now));
	}

	exit(igt_exitcode);
}

/* fork support code */
static int helper_process_count;
static pid_t helper_process_pids[] =
{ -1, -1, -1, -1};

static void reset_helper_process_list(void)
{
	for (int i = 0; i < ARRAY_SIZE(helper_process_pids); i++)
		helper_process_pids[i] = -1;
	helper_process_count = 0;
}

static int __waitpid(pid_t pid)
{
	int status = -1;
	while (waitpid(pid, &status, 0) == -1 &&
	       errno == EINTR)
		;

	return status;
}

static void fork_helper_exit_handler(int sig)
{
	/* Inside a signal handler, play safe */
	for (int i = 0; i < ARRAY_SIZE(helper_process_pids); i++) {
		pid_t pid = helper_process_pids[i];
		if (pid != -1) {
			kill(pid, SIGTERM);
			__waitpid(pid);
			helper_process_count--;
		}
	}

	assert(helper_process_count == 0);
}

bool __igt_fork_helper(struct igt_helper_process *proc)
{
	pid_t pid;
	int id;
	int tmp_count;

	assert(!proc->running);
	assert(helper_process_count < ARRAY_SIZE(helper_process_pids));

	for (id = 0; helper_process_pids[id] != -1; id++)
		;

	igt_install_exit_handler(fork_helper_exit_handler);

	/*
	 * Avoid races when the parent stops the child before the setup code
	 * had a chance to run. This happens e.g. when skipping tests wrapped in
	 * the signal helper.
	 */
	tmp_count = exit_handler_count;
	exit_handler_count = 0;

	/* ensure any buffers are flushed before fork */
	fflush(NULL);

	switch (pid = fork()) {
	case -1:
		exit_handler_count = tmp_count;
		igt_assert(0);
	case 0:
		reset_helper_process_list();
		oom_adjust_for_doom();

		return true;
	default:
		exit_handler_count = tmp_count;
		proc->running = true;
		proc->pid = pid;
		proc->id = id;
		helper_process_pids[id] = pid;
		helper_process_count++;

		return false;
	}

}

/**
 * igt_wait_helper:
 * @proc: #igt_helper_process structure
 *
 * Joins a helper process. It is an error to call this on a helper process which
 * hasn't been spawned yet.
 */
int igt_wait_helper(struct igt_helper_process *proc)
{
	int status;

	assert(proc->running);

	status = __waitpid(proc->pid);

	proc->running = false;

	helper_process_pids[proc->id] = -1;
	helper_process_count--;

	return status;
}

static bool helper_was_alive(struct igt_helper_process *proc,
			     int status)
{
	return (WIFSIGNALED(status) &&
		WTERMSIG(status) == (proc->use_SIGKILL ? SIGKILL : SIGTERM));
}

/**
 * igt_stop_helper:
 * @proc: #igt_helper_process structure
 *
 * Terminates a helper process. It is an error to call this on a helper process
 * which hasn't been spawned yet.
 */
void igt_stop_helper(struct igt_helper_process *proc)
{
	int status;

	/* failure here means the pid is already dead and so waiting is safe */
	kill(proc->pid, proc->use_SIGKILL ? SIGKILL : SIGTERM);

	status = igt_wait_helper(proc);
	if (!helper_was_alive(proc, status))
		igt_debug("Helper died too early with status=%d\n", status);
	assert(helper_was_alive(proc, status));
}

static void children_exit_handler(int sig)
{
	int status;

	/* The exit handler can be called from a fatal signal, so play safe */
	while (num_test_children-- && wait(&status))
		;
}

bool __igt_fork(void)
{
	assert(!test_with_subtests || in_subtest);
	assert(!test_child);

	igt_install_exit_handler(children_exit_handler);

	if (num_test_children >= test_children_sz) {
		if (!test_children_sz)
			test_children_sz = 4;
		else
			test_children_sz *= 2;

		test_children = realloc(test_children,
					sizeof(pid_t)*test_children_sz);
		igt_assert(test_children);
	}

	/* ensure any buffers are flushed before fork */
	fflush(NULL);

	switch (test_children[num_test_children++] = fork()) {
	case -1:
		igt_assert(0);
	case 0:
		test_child = true;
		exit_handler_count = 0;
		reset_helper_process_list();
		oom_adjust_for_doom();

		return true;
	default:
		return false;
	}

}

/**
 * igt_waitchildren:
 *
 * Wait for all children forked with igt_fork.
 *
 * The magic here is that exit codes from children will be correctly propagated
 * to the main thread, including the relevant exit code if a child thread failed.
 * Of course if multiple children failed with different exit codes the resulting
 * exit code will be non-deterministic.
 *
 * Note that igt_skip() will not be forwarded, feature tests need to be done
 * before spawning threads with igt_fork().
 */
void igt_waitchildren(void)
{
	int err = 0;
	int count;

	assert(!test_child);

	count = 0;
	while (count < num_test_children) {
		int status = -1;
		pid_t pid;
		int c;

		pid = wait(&status);
		if (pid == -1)
			continue;

		for (c = 0; c < num_test_children; c++)
			if (pid == test_children[c])
				break;
		if (c == num_test_children)
			continue;

		if (err == 0 && status != 0) {
			if (WIFEXITED(status)) {
				printf("child %i failed with exit status %i\n",
				       c, WEXITSTATUS(status));
				err = WEXITSTATUS(status);
			} else if (WIFSIGNALED(status)) {
				printf("child %i died with signal %i, %s\n",
				       c, WTERMSIG(status),
				       strsignal(WTERMSIG(status)));
				err = 128 + WTERMSIG(status);
			} else {
				printf("Unhandled failure [%d] in child %i\n", status, c);
				err = 256;
			}

			for (c = 0; c < num_test_children; c++)
				kill(test_children[c], SIGKILL);
		}

		count++;
	}

	num_test_children = 0;
	if (err)
		igt_fail(err);
}

/**
 * igt_waitchildren_timeout:
 * @seconds: timeout in seconds to wait
 * @reason: debug string explaining what timedout
 *
 * Wait for all children forked with igt_fork, for a maximum of @seconds.
 *
 * Wraps igt_waitchildren() and igt_set_timeout()
 */
void igt_waitchildren_timeout(int seconds, const char *reason)
{
	igt_set_timeout(seconds, reason);
	igt_waitchildren();
	igt_reset_timeout();
}

/* exit handler code */
#define MAX_SIGNALS		32
#define MAX_EXIT_HANDLERS	10

#ifndef HAVE_SIGHANDLER_T
typedef void (*sighandler_t)(int);
#endif

static struct {
	sighandler_t handler;
	bool installed;
} orig_sig[MAX_SIGNALS];

static igt_exit_handler_t exit_handler_fn[MAX_EXIT_HANDLERS];
static bool exit_handler_disabled;
#define SIGDEF(x) { x, #x, sizeof(#x) - 1 }
#define SILENT(x) { x, NULL, 0 }
static const struct { int number; const char *name; size_t name_len; } handled_signals[] =
	{ SILENT(SIGINT), SILENT(SIGHUP), SILENT(SIGTERM), SILENT(SIGQUIT),
	  SILENT(SIGPIPE), SIGDEF(SIGABRT), SIGDEF(SIGSEGV), SIGDEF(SIGBUS),
	  SIGDEF(SIGFPE) };
#undef SILENT
#undef SIGDEF

static int install_sig_handler(int sig_num, sighandler_t handler)
{
	orig_sig[sig_num].handler = signal(sig_num, handler);

	if (orig_sig[sig_num].handler == SIG_ERR)
		return -1;

	orig_sig[sig_num].installed = true;

	return 0;
}

static void restore_sig_handler(int sig_num)
{
	/* Just restore the default so that we properly fall over. */
	signal(sig_num, SIG_DFL);
}

static void restore_all_sig_handler(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(orig_sig); i++)
		restore_sig_handler(i);
}

static void call_exit_handlers(int sig)
{
	int i;

	if (!exit_handler_count) {
		return;
	}

	for (i = exit_handler_count - 1; i >= 0; i--)
		exit_handler_fn[i](sig);

	/* ensure we don't get called twice */
	exit_handler_count = 0;
}

static void igt_atexit_handler(void)
{
	in_atexit_handler = true;

	restore_all_sig_handler();

	if (!exit_handler_disabled)
		call_exit_handlers(0);
}

static bool crash_signal(int sig)
{
	switch (sig) {
	case SIGILL:
	case SIGBUS:
	case SIGFPE:
	case SIGSEGV:
		return true;
	default:
		return false;
	}
}

static void fatal_sig_handler(int sig)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(handled_signals); i++) {
		if (handled_signals[i].number != sig)
			continue;

		if (handled_signals[i].name_len) {
			igt_assert_eq(write(STDERR_FILENO, "Received signal ", 16),
                                      16);
			igt_assert_eq(write(STDERR_FILENO, handled_signals[i].name, handled_signals[i].name_len),
                                      handled_signals[i].name_len);
			igt_assert_eq(write(STDERR_FILENO, ".\n", 2), 2);
		}

		if (crash_signal(sig)) {
			/* Linux standard to return exit code as 128 + signal */
			if (!failed_one)
				igt_exitcode = 128 + sig;

			failed_one = true;
#ifdef HAVE_LIBUNWIND
			print_backtrace_sig_safe();
#endif
			if (in_subtest)
				exit_subtest("CRASH");
		}
		break;
	}

	restore_all_sig_handler();

	/*
	 * exit_handler_disabled is always false here, since when we set it
	 * we also block signals.
	 */
	call_exit_handlers(sig);

	{
#ifdef __linux__
	/* Workaround cached PID and TID races on glibc and Bionic libc. */
		pid_t pid = syscall(SYS_getpid);
		pid_t tid = syscall(SYS_gettid);

		syscall(SYS_tgkill, pid, tid, sig);
#else
		pthread_t tid = pthread_self();
		union sigval value = { .sival_ptr = NULL };

#ifndef __FreeBSD__
		pthread_sigqueue(tid, sig, value);
#else
		pthread_kill(tid, sig);
#endif
#endif
        }
}

/**
 * igt_install_exit_handler:
 * @fn: exit handler function
 *
 * Set a handler that will be called either when the process calls exit() or
 * <!-- -->returns from the main function, or one of the signals in
 * 'handled_signals' is raised. MAX_EXIT_HANDLERS handlers can be installed,
 * each of which will be called only once, even if a subsequent signal is
 * raised. If the exit handlers are called due to a signal, the signal will be
 * re-raised with the original signal disposition after all handlers returned.
 *
 * The handler will be passed the signal number if called due to a signal, or
 * 0 otherwise. Exit handlers can also be used from test children spawned with
 * igt_fork(), but not from within helper processes spawned with
 * igt_fork_helper(). The list of exit handlers is reset when forking to
 * avoid issues with children cleanup up the parent's state too early.
 */
void igt_install_exit_handler(igt_exit_handler_t fn)
{
	int i;

	for (i = 0; i < exit_handler_count; i++)
		if (exit_handler_fn[i] == fn)
			return;

	igt_assert(exit_handler_count < MAX_EXIT_HANDLERS);

	exit_handler_fn[exit_handler_count] = fn;
	exit_handler_count++;

	if (exit_handler_count > 1)
		return;

	for (i = 0; i < ARRAY_SIZE(handled_signals); i++) {
		if (install_sig_handler(handled_signals[i].number,
					fatal_sig_handler))
			goto err;
	}

	if (atexit(igt_atexit_handler))
		goto err;

	return;
err:
	restore_all_sig_handler();
	exit_handler_count--;

	igt_assert_f(0, "failed to install the signal handler\n");
}

/* simulation enviroment support */

/**
 * igt_run_in_simulation:
 *
 * This function can be used to select a reduced test set when running in
 * simulation environments. This i-g-t mode is selected by setting the
 * INTEL_SIMULATION environment variable to 1.
 *
 * Returns: True when run in simulation mode, false otherwise.
 */
bool igt_run_in_simulation(void)
{
	static int simulation = -1;

	if (simulation == -1)
		simulation = igt_check_boolean_env_var("INTEL_SIMULATION", false);

	return simulation;
}

/**
 * igt_skip_on_simulation:
 *
 * Skip tests when INTEL_SIMULATION environment variable is set. It uses
 * igt_skip() internally and hence is fully subtest aware.
 *
 * Note that in contrast to all other functions which use igt_skip() internally
 * it is allowed to use this outside of an #igt_fixture block in a test with
 * subtests. This is because in contrast to most other test requirements,
 * checking for simulation mode doesn't depend upon the present hardware and it
 * so makes a lot of sense to have this check in the outermost #igt_main block.
 */
void igt_skip_on_simulation(void)
{
	if (igt_only_list_subtests())
		return;

	if (!in_fixture && !in_subtest) {
		igt_fixture
			igt_require(!igt_run_in_simulation());
	} else
		igt_require(!igt_run_in_simulation());
}

/* structured logging */

/**
 * igt_log:
 * @domain: the log domain, or NULL for no domain
 * @level: #igt_log_level
 * @format: format string
 * @...: optional arguments used in the format string
 *
 * This is the generic structured logging helper function. i-g-t testcase should
 * output all normal message to stdout. Warning level message should be printed
 * to stderr and the test runner should treat this as an intermediate result
 * between SUCCESS and FAILURE.
 *
 * The log level can be set through the IGT_LOG_LEVEL environment variable with
 * values "debug", "info", "warn", "critical" and "none". By default verbose
 * debug message are disabled. "none" completely disables all output and is not
 * recommended since crucial issues only reported at the IGT_LOG_WARN level are
 * ignored.
 */
void igt_log(const char *domain, enum igt_log_level level, const char *format, ...)
{
	va_list args;

	va_start(args, format);
	igt_vlog(domain, level, format, args);
	va_end(args);
}

/**
 * igt_vlog:
 * @domain: the log domain, or NULL for no domain
 * @level: #igt_log_level
 * @format: format string
 * @args: variable arguments lists
 *
 * This is the generic logging helper function using an explicit varargs
 * structure and hence useful to implement domain-specific logging
 * functions.
 *
 * If there is no need to wrap up a vararg list in the caller it is simpler to
 * just use igt_log().
 */
void igt_vlog(const char *domain, enum igt_log_level level, const char *format, va_list args)
{
	FILE *file;
	char *line, *formatted_line;
	const char *program_name;
	const char *igt_log_level_str[] = {
		"DEBUG",
		"INFO",
		"WARNING",
		"CRITICAL",
		"NONE"
	};
	static bool line_continuation = false;

	assert(format);

#ifdef __GLIBC__
	program_name = program_invocation_short_name;
#else
	program_name = command_str;
#endif

	if (list_subtests && level <= IGT_LOG_WARN)
		return;

	if (vasprintf(&line, format, args) == -1)
		return;

	if (line_continuation) {
		formatted_line = strdup(line);
		if (!formatted_line)
			goto out;
	} else if (asprintf(&formatted_line, "(%s:%d) %s%s%s: %s", program_name,
		     getpid(), (domain) ? domain : "", (domain) ? "-" : "",
		     igt_log_level_str[level], line) == -1) {
		goto out;
	}

	line_continuation = line[strlen(line) - 1] != '\n';

	/* append log buffer */
	_igt_log_buffer_append(formatted_line);

	/* check print log level */
	if (igt_log_level > level)
		goto out;

	/* check domain filter */
	if (igt_log_domain_filter) {
		/* if null domain and filter is not "application", return */
		if (!domain && strcmp(igt_log_domain_filter, "application"))
			goto out;
		/* else if domain and filter do not match, return */
		else if (domain && strcmp(igt_log_domain_filter, domain))
			goto out;
	}

	/* use stderr for warning messages and above */
	if (level >= IGT_LOG_WARN) {
		file = stderr;
		fflush(stdout);
	}
	else
		file = stdout;

	/* prepend all except information messages with process, domain and log
	 * level information */
	if (level != IGT_LOG_INFO)
		fwrite(formatted_line, sizeof(char), strlen(formatted_line),
		       file);
	else
		fwrite(line, sizeof(char), strlen(line), file);

out:
	free(line);
}

static const char *timeout_op;
static void __attribute__((noreturn)) igt_alarm_handler(int signal)
{
	if (timeout_op)
		igt_info("Timed out: %s\n", timeout_op);
	else
		igt_info("Timed out\n");

	/* exit with failure status */
	igt_fail(IGT_EXIT_FAILURE);
}

/**
 * igt_set_timeout:
 * @seconds: number of seconds before timeout
 * @op: Optional string to explain what operation has timed out in the debug log
 *
 * Fail a test and exit with #IGT_EXIT_FAILURE status after the specified
 * number of seconds have elapsed. If the current test has subtests and the
 * timeout occurs outside a subtest, subsequent subtests will be skipped and
 * marked as failed.
 *
 * Any previous timer is cancelled and no timeout is scheduled if @seconds is
 * zero. But for clarity the timeout set with this function should be cleared
 * with igt_reset_timeout().
 */
void igt_set_timeout(unsigned int seconds,
		     const char *op)
{
	struct sigaction sa;

	sa.sa_handler = igt_alarm_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;

	timeout_op = op;

	if (seconds == 0)
		sigaction(SIGALRM, NULL, NULL);
	else
		sigaction(SIGALRM, &sa, NULL);

	alarm(seconds);
}

/**
 * igt_reset_timeout:
 *
 * This function resets a timeout set by igt_set_timeout() and disables any
 * timer set up by the former function.
 */
void igt_reset_timeout(void)
{
	igt_set_timeout(0, NULL);
}

FILE *__igt_fopen_data(const char* igt_srcdir, const char* igt_datadir,
		       const char* filename)
{
	char path[PATH_MAX];
	FILE *fp;

	snprintf(path, sizeof(path), "%s/%s", igt_datadir, filename);
	fp = fopen(path, "r");
	if (!fp) {
		snprintf(path, sizeof(path), "%s/%s", igt_srcdir, filename);
		fp = fopen(path, "r");
	}
	if (!fp) {
		snprintf(path, sizeof(path), "./%s", filename);
		fp = fopen(path, "r");
	}

	if (!fp)
		igt_critical("Could not open data file \"%s\": %s", filename,
			     strerror(errno));

	return fp;
}
