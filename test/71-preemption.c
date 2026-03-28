#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/time.h>
#include <inttypes.h>
#include <sys/time.h>
#include "thread.h"

/* test de validation de la pré-emption (et par effet de bord de certaines implémentation de priorités)
 *
 * valgrind doit etre content.
 * La durée du programme doit etre proportionnelle au nombre de threads donnés en argument.
 * L'affichage doit être une série d'id alternée dans un ordre plus ou
 * moin aléatoire, et non une suite de 0, puis de 1, puis de 2, ...
 *
 * support nécessaire:
 * - thread_create()
 * - retour sans thread_exit()
 * - thread_join() sans récupération de la valeur de retour
 */
#define NB_ITER     100
#define ITER_LENGTH 1000000

static int    fini  = 0;
static double score = 0;
static long  *values = NULL;

static void * thfunc( void *arg )
{
    unsigned long i, j = 0;
    int me = (intptr_t)arg;

    for(i=0; i<NB_ITER;i++) {
	for(j=0; j<ITER_LENGTH;j++) {
	    if (fini) {
		return NULL;
	    }
	    values[me]++;
	}
	fprintf(stderr, "%ld ", (intptr_t)arg );
    }
    fini = 1;

    return NULL;
}

int main(int argc, char *argv[])
{
    thread_t *th;
    int i, err, nb;
    struct timeval tv1, tv2;
    unsigned long us;

    if (argc < 2) {
        printf("argument manquant: nombre de threads\n");
        return EXIT_FAILURE;
    }

    nb = atoi(argv[1]);
    th = malloc(nb * sizeof(*th));
    if (!th) {
        perror("malloc(th)");
        return EXIT_FAILURE;
    }

    values = calloc( nb+1, sizeof(long) );
    if (!values) {
        perror("malloc(values)");
        return EXIT_FAILURE;
    }

    gettimeofday(&tv1, NULL);
    /* on cree tous les threads */
    for(i=0; i<nb; i++) {
        err = thread_create(&th[i], thfunc, (void*)((intptr_t)i));
        assert(!err);
    }

    /* On participe au réchauffement climatique */
    thfunc( (void*)((intptr_t)nb) );

    /* on les join tous, maintenant qu'ils sont tous morts */
    score = values[nb];
    for(i=0; i<nb; i++) {
        err = thread_join(th[i], NULL);
        assert(!err);

	score += values[i];
    }

    gettimeofday(&tv2, NULL);
    us = (tv2.tv_sec-tv1.tv_sec)*1000000+(tv2.tv_usec-tv1.tv_usec);

    score = score / ( (double)(nb+1) * NB_ITER * ITER_LENGTH );
    printf("\nscore: %lf\n", score );

    printf("Programme exécuté en %ld us\n", us);

    free(th);
    free(values);

    if ( score < .5 ) {
	return EXIT_FAILURE;
    }
    else {
	printf("Temps attendu pour le programme complet: %e us\n", us / score );
	return EXIT_SUCCESS;
    }
}