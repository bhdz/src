/*	$Id: test-cert.c,v 1.13 2021/10/13 06:56:07 claudio Exp $ */
/*
 * Copyright (c) 2019 Kristaps Dzonsons <kristaps@bsd.lv>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/socket.h>
#include <arpa/inet.h>

#include <assert.h>
#include <err.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/x509v3.h>

#include "extern.h"

#include "test-common.c"

int verbose;

static void
cert_print(const struct cert *p)
{
	size_t	 i;
	char	 buf1[64], buf2[64];
	int	 sockt;
	char	 tbuf[21];

	assert(p != NULL);

	printf("Subject key identifier: %s\n", pretty_key_id(p->ski));
	if (p->aki != NULL)
		printf("Authority key identifier: %s\n", pretty_key_id(p->aki));
	if (p->aia != NULL)
		printf("Authority info access: %s\n", p->aia);
	if (p->mft != NULL)
		printf("Manifest: %s\n", p->mft);
	if (p->repo != NULL)
		printf("caRepository: %s\n", p->repo);
	if (p->notify != NULL)
		printf("Notify URL: %s\n", p->notify);
	if (p->pubkey != NULL)
		printf("BGPsec P-256 ECDSA public key: %s\n", p->pubkey);
	strftime(tbuf, sizeof(tbuf), "%FT%TZ", gmtime(&p->expires));
	printf("Valid until: %s\n", tbuf);

	printf("Subordinate Resources:\n");

	for (i = 0; i < p->asz; i++)
		switch (p->as[i].type) {
		case CERT_AS_ID:
			printf("%5zu: AS: %"
				PRIu32 "\n", i + 1, p->as[i].id);
			break;
		case CERT_AS_INHERIT:
			printf("%5zu: AS: inherit\n", i + 1);
			break;
		case CERT_AS_RANGE:
			printf("%5zu: AS: %"
				PRIu32 "--%" PRIu32 "\n", i + 1,
				p->as[i].range.min, p->as[i].range.max);
			break;
		}

	for (i = 0; i < p->ipsz; i++)
		switch (p->ips[i].type) {
		case CERT_IP_INHERIT:
			printf("%5zu: IP: inherit\n", i + 1);
			break;
		case CERT_IP_ADDR:
			ip_addr_print(&p->ips[i].ip,
				p->ips[i].afi, buf1, sizeof(buf1));
			printf("%5zu: IP: %s\n", i + 1, buf1);
			break;
		case CERT_IP_RANGE:
			sockt = (p->ips[i].afi == AFI_IPV4) ?
				AF_INET : AF_INET6;
			inet_ntop(sockt, p->ips[i].min, buf1, sizeof(buf1));
			inet_ntop(sockt, p->ips[i].max, buf2, sizeof(buf2));
			printf("%5zu: IP: %s--%s\n", i + 1, buf1, buf2);
			break;
		}

}

int
main(int argc, char *argv[])
{
	int		 c, i, verb = 0, ta = 0;
	X509		*xp = NULL;
	struct cert	*p;

	ERR_load_crypto_strings();
	OpenSSL_add_all_ciphers();
	OpenSSL_add_all_digests();

	while ((c = getopt(argc, argv, "tv")) != -1)
		switch (c) {
		case 't':
			ta = 1;
			break;
		case 'v':
			verb++;
			break;
		default:
			errx(1, "bad argument %c", c);
		}

	argv += optind;
	argc -= optind;

	if (argc == 0)
		errx(1, "argument missing");

	if (ta) {
		if (argc % 2)
			errx(1, "need even number of arguments");

		for (i = 0; i < argc; i += 2) {
			const char	*cert_path = argv[i];
			const char	*tal_path = argv[i + 1];
			char		*buf;
			struct tal	*tal;

			buf = tal_read_file(tal_path);
			tal = tal_parse(tal_path, buf);
			free(buf);
			if (tal == NULL)
				break;

			p = ta_parse(&xp, cert_path, tal->pkey, tal->pkeysz);
			tal_free(tal);
			if (p == NULL)
				break;

			if (verb)
				cert_print(p);
			cert_free(p);
			X509_free(xp);
		}
	} else {
		for (i = 0; i < argc; i++) {
			p = cert_parse(&xp, argv[i]);
			if (p == NULL)
				break;
			if (verb)
				cert_print(p);
			cert_free(p);
			X509_free(xp);
		}
	}

	EVP_cleanup();
	CRYPTO_cleanup_all_ex_data();
	ERR_free_strings();

	if (i < argc)
		errx(1, "test failed for %s", argv[i]);

	printf("OK\n");
	return 0;
}
