# Analyse comparative: avant/après le switch de contexte assembleur

Cette analyse compare:
- **Baseline**: parent de la révision `003e3c1` (`f752115`), implémentation basée sur `swapcontext`.
- **Implémentation ASM**: révision `003e3c1`, remplacement par un switch de contexte minimal en assembleur (`src/context.S`, `include/context.h`).

## Pourquoi cette comparaison

La révision `003e3c1` introduit un mécanisme de changement de contexte qui:
- sauvegarde/restaure uniquement les registres callee-saved + SP + PC,
- évite le coût de `sigprocmask` intrinsèque à `swapcontext`/`setcontext`,
- réduit le coût unitaire des `thread_yield` et des transitions de scheduling.

L’objectif est de vérifier **quantitativement** l’impact sur des tests dominés par les switches de contexte.

## Méthodologie

- Deux worktrees Git temporaires sont créés automatiquement:
  - `baseline = 003e3c1^` (`f752115`)
  - `impl_asm = 003e3c1`
- Chaque worktree est compilé avec `make clean && make all pthreads`.
- Benchmarks exécutés: `31-switch-many`, `32-switch-many-join`, `33-switch-many-cascade`, `sum`.
- 4 exécutions par test et par version; comparaison sur la **médiane** du temps mur.

Script utilisé:
- `scripts/compare_context_switch_impl_asm.py`

Commande de reproduction:

```bash
/home/amrar/Rafiq/2A/S8/projSYSexp/ein8-proj1-28839/.venv/bin/python scripts/compare_context_switch_impl_asm.py \
  --repo /home/amrar/Rafiq/2A/S8/projSYSexp/ein8-proj1-28839 \
  --target 003e3c1 \
  --runs 4 \
  --output-dir analysis/builds/06_context_switch_asm
```

## Résultats

Source: `results.csv`

| Test | Args | Avant (s) | Après (s) | Speedup |
|---|---:|---:|---:|---:|
| 31-switch-many | 200 200 | 0.044735 | 0.006235 | **7.174x** |
| 32-switch-many-join | 200 200 | 0.004180 | 0.004421 | 0.945x |
| 33-switch-many-cascade | 120 150 | 0.935531 | 0.038416 | **24.353x** |
| sum | - | 0.353287 | 0.322523 | 1.095x |

Graphes générés:
- `runtime_comparison.png`
- `speedup.png`

## Interprétation

- Les workloads **fortement dépendants des yields/switches** progressent fortement:
  - `31-switch-many`: ~7.17x
  - `33-switch-many-cascade`: ~24.35x
- `sum` (moins dominé par le switch pur) montre un gain modéré (~1.10x), ce qui est cohérent.
- `32-switch-many-join` est quasi stable (légère régression dans cette campagne), probablement plus sensible au coût du join/scheduling global et au bruit de mesure à très faible durée absolue.

## Conclusion

La révision `003e3c1` apporte un gain majeur sur le cœur du runtime utilisateur (switch de contexte), particulièrement visible dans les scénarios de haute fréquence de `thread_yield`. Les résultats confirment que l’abandon de `swapcontext` au profit du chemin assembleur minimal atteint l’objectif de performance.
