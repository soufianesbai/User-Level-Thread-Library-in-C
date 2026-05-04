Graph Exec Time Comparison :


- Exemple de commande pour compiler le test 21 avec plusieurs coeurs:
	make graphs ARGS="--test 21-create-many --cores 1,2,4,8 --runs 3 --png graph_exec_time_comparison/bench/21.png"

- Exemple de commande de compilation pour fibonacci progressif (on a un arrêt automatique si time>timeout ou echec):
	make graphs ARGS="--test 51-fibonacci --fibonacci-start 1 --fibonacci-max 45 --fibonacci-step 1 --timeout 20 --runs 2 --png graph_exec_time_comparison/bench/51-fibonacci.png"

- Exemple de commande de compilation pour test 33 avec plusiurs threads et plusieurs coeurs (quand on fait varier l'un l'autre est fixe si variation de thread coeurs fixe à 8 si variation de coeurs threads fixe à 100)
    make graphs ARGS="--test 33-switch-many-cascade --png graph_exec_time_comparison/bench/33.png --cores 1,2,4,8 --threads 10,50,100,200 --runs 3"

Arguments benchmark_plot.py
- --repo
	Racine du projet. Defaut: .

- --runs
	Nombre de repetitions par point. Defaut: 2.

- --timeout
	Timeout en secondes pour une execution. Defaut: 20.

- --png
	Fichier image de sortie.
	Si chemin relatif, il est force vers graph_exec_time_comparison/bench.
	Exemple: --png 21.png cree graph_exec_time_comparison/bench/21.png

- --cores
	Liste de coeurs separes par virgule pour le graphe scaling coeurs.
	Exemple: --cores 1,2,4,8

- --threads
	Liste de tailles de threads (tests 20/30 compatibles).
	Exemple: --threads 10,50,100,200

- --fibonacci-start
	Valeur de depart pour le sweep fibonacci. Defaut: 1.

- --fibonacci-max
	Valeur max du sweep fibonacci. Defaut: 45.

- --fibonacci-step
	Pas du sweep fibonacci. Defaut: 1.

- --test
	Nom d un test unique a executer.
	Exemple: 21-create-many, 33-switch-many-cascade, 51-fibonacci

- --skip-build
	Ne relance pas la compilation avant benchmark.

- --custom-tests
	Active le mode benchmark custom. Requis en appel direct python.
	Avec make graphs, il est deja ajoute automatiquement.
