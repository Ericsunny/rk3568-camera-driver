/* Compare actual event-name construction with the pre-C9 truncation behavior. */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static FILE *fake_fopen(const char *, const char *);
#define fopen fake_fopen
#define main evrec_original_main
#include "../evrec.c"
#undef main
#undef fopen

static FILE *fake_fopen(const char *path, const char *mode)
{
	(void)path; (void)mode; errno = EACCES; return NULL;
}

int main(void)
{
	const int lengths[] = {0, 1, 300, 500, 510, 511};
	const unsigned serials[] = {1, 99, UINT_MAX};
	const unsigned segments[] = {0, 1, UINT_MAX};
	char expected[sizeof(g_ev_path)];
	size_t i, j, k;

	for (i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
		memset(g_out_base, 'a', (size_t)lengths[i]);
		g_out_base[lengths[i]] = '\0';
		for (j = 0; j < sizeof(serials) / sizeof(serials[0]); j++) {
			for (k = 0; k < sizeof(segments) / sizeof(segments[0]); k++) {
				if (segments[k] == 0)
					snprintf(expected, sizeof(expected), "%s-%02u", g_out_base, serials[j]);
				else
					snprintf(expected, sizeof(expected), "%s-%02u-s%u", g_out_base,
						 serials[j], segments[k]);
				(void)evw_open_at(0, serials[j], segments[k], 1);
				if (strcmp(expected, g_ev_path) != 0) {
					fprintf(stderr, "FAIL event name length=%d serial=%u segment=%u\n",
						lengths[i], serials[j], segments[k]);
					return 1;
				}
			}
		}
	}
	puts("event names retain prior truncation behavior: PASS");
	return 0;
}
