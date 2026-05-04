# Bibliothèque de threads en espace utilisateur

Implémentation d'une bibliothèque de threads POSIX-compatible en C, avec gestion coopérative de l'ordonnancement, primitives de synchronisation, détection de deadlocks et support optionnel de la préemption.

## Architecture

- `src/thread.c` — création, jointure et terminaison de threads (`thread_create`, `thread_join`, `thread_exit`)
- `src/scheduler.c` — ordonnanceur (FIFO et priorités) et pool de threads de travail
- `src/thread_mutex.c` — mutex avec détection de deadlock
- `src/thread_sem.c` — sémaphores
- `src/thread_cond.c` — variables de condition
- `src/thread_sig.c` — signaux inter-threads (`thread_signal_send`, `thread_sigwait`)
- `src/preemption.c` — préemption par timer POSIX (`SIGALRM`)
- `src/context.S` — sauvegarde/restauration de contexte bas niveau (`fast_swap_context`)
- `src/pool.c` — pool de threads pour parallélisme fork/join

## API principale

```c
/* Cycle de vie */
int  thread_create(thread_t *th, void *(*func)(void *), void *arg);
int  thread_join(thread_t th, void **retval);
void thread_exit(void *retval);

/* Ordonnancement */
thread_t thread_self(void);
int      thread_yield(void);
int      thread_yield_to(thread_t target);
int      thread_set_priority(thread_t t, int prio);   /* THREAD_SCHED_PRIO uniquement */

/* Mutex */
int thread_mutex_init(thread_mutex_t *m);
int thread_mutex_lock(thread_mutex_t *m);
int thread_mutex_unlock(thread_mutex_t *m);
int thread_mutex_destroy(thread_mutex_t *m);

/* Sémaphores */
int thread_sem_init(thread_sem_t *s, int value);
int thread_sem_wait(thread_sem_t *s);
int thread_sem_post(thread_sem_t *s);
int thread_sem_destroy(thread_sem_t *s);

/* Variables de condition */
int thread_cond_init(thread_cond_t *c);
int thread_cond_wait(thread_cond_t *c, thread_mutex_t *m);
int thread_cond_signal(thread_cond_t *c);
int thread_cond_broadcast(thread_cond_t *c);
int thread_cond_destroy(thread_cond_t *c);

/* Signaux inter-threads (nécessite -DENABLE_SIGNAL) */
int thread_signal_send(thread_t target, int sig);
int thread_sigwait(thread_sigset_t set, int *sig);
```

## Compilation

```bash
make          # bibliothèque + tous les tests
make check    # compile et exécute la suite de tests
make clean    # supprime les artefacts
```

### Options de compilation

| Variable | Valeurs | Défaut | Effet |
|---|---|---|---|
| `THREAD_SCHED_POLICY` | `THREAD_SCHED_FIFO` / `THREAD_SCHED_PRIO` | `FIFO` | Politique d'ordonnancement |
| `THREAD_ENABLE_GUARD_PAGE` | `0` / `1` | `0` | Page garde en bas de pile |
| `THREAD_ENABLE_OVERFLOW_DETECTION` | `0` / `1` | `0` | Détection de débordement de pile |
| `ENABLE_SIGNAL` | `0` / `1` | `0` | Support des signaux inter-threads |

Exemple :
```bash
make THREAD_SCHED_POLICY=THREAD_SCHED_PRIO THREAD_ENABLE_OVERFLOW_DETECTION=1
```

### Lien avec pthread (pour comparaison)

Chaque test peut être compilé avec `-DUSE_PTHREAD` pour utiliser les pthreads natifs à la place de cette bibliothèque :

```bash
make pthreads   # produit bin/*-pthread
```

## Tests

| Préfixe | Contenu |
|---|---|
| `01–03` | Démarrage, yield, équité |
| `11–13` | Jointure |
| `21–23` | Création massive de threads |
| `31–33` | Yield en masse |
| `51` | Fibonacci récursif (fork/join) |
| `61–68` | Mutex, sémaphore, condition, erreurs |
| `71` | Préemption |
| `72` | Détection de débordement de pile |
| `81–82` | Détection de deadlock |
| `matrix_mul`, `reduction`, `sort`, `sum` | Benchmarks de calcul |

## Benchmarks

```bash
make graphs ARGS="--custom-tests"
# options utiles :
#   --cores 1,2,4    limiter les cœurs (taskset)
#   --runs 3         répétitions par mesure
#   --test sort      un seul test
```

Les graphes sont générés dans `graph_exec_time_comparison/bench/`.

## Branche multicoeur (`multiz`)

La branche **`multiz`** étend cette bibliothèque avec un vrai parallélisme multi-cœurs :

- **Pool de workers POSIX** : N threads POSIX (pthreads) exécutent les threads utilisateur en parallèle sur autant de cœurs physiques.
- **Ordonnanceur partagé** : file de threads prêts protégée par un mutex global, broadcast sur condition pour réveiller les workers idle.
- **Contexte de jointure différée** : évite la race condition où `thread_exit` pourrait réenqueuer un joiner avant que `fast_swap_context` ait fini de sauvegarder ses registres.
- **`thread_set_concurrency(n)`** : choisit dynamiquement le nombre de workers au démarrage.
- **Bibliothèque monocoeur séparée** (`libthread_mono.so`) : variante sans `THREAD_MULTICORE` pour la comparaison en courbes.
- **Tests multicoeur avancés** : épinglage de threads, fibonacci avec cutoff séquentiel, deadlock multicoeur, variables de condition sous contention.

```bash
git checkout multiz
make                          # compile libthread.so (multicoeur) + libthread_mono.so
make check                    # suite de tests complète
make graphs ARGS="--custom-tests"   # courbes monocoeur / multicoeur / pthread
```
