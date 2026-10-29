# Export groupé des diagnostics — 29 octobre 2026

Appliquer le contenu du dossier `strato/` de cette archive à la racine du projet,
en remplaçant les fichiers correspondants, puis reconstruire l’APK.
Cette livraison contient uniquement les fichiers modifiés ou ajoutés pour cette option.
Elle part du `strato.zip` fourni (SHA-256 :
`2e22033e6ae26daa05e1aaaa8c2a3f7982139daa9ff94534e2b9f155b11f3631`).

## Utilisation

Dans Diagnostic, appuyer sur **Exporter plusieurs diagnostics** :

- **Sélectionner les diagnostics** : cocher librement les sessions.
- **Aujourd’hui** : présélectionner les sessions démarrées aujourd’hui.
- **7 derniers jours (aujourd’hui inclus)** : présélectionner les sessions des sept dates locales les plus récentes.
- **Tout sélectionner** : présélectionner toutes les sessions enregistrées.

Chaque raccourci ouvre la liste à cocher ; la sélection reste modifiable.
Le bouton **Exporter (N)** indique le nombre de sessions retenues et reste désactivé
si rien n’est sélectionné. Un bouton permet de tout sélectionner/désélectionner.
La sélection est conservée lors d’une rotation de l’écran.
Les périodes utilisent les dates de début des sessions et le fuseau du téléphone.

Le partage envoie un seul ZIP contenant `diagnostic-1.zip`, `diagnostic-2.zip`, etc.
Chaque ZIP interne conserve le format existant, son `session.json` et son
`manifest.json` avec le jeu, l’identité de session et les limites de collecte.
Extraire le ZIP externe pour utiliser les outils d’analyse existants sur un rapport.
L’export individuel reste disponible.

Les rapports sont traités successivement sur le thread d’entrée/sortie. Un seul
ZIP de session temporaire est conservé à la fois, en plus du lot en cours.
Les sessions originales ne sont pas supprimées. Les sessions actives restent des
instantanés non atomiques, signalés dans leur manifeste.

Le lot est limité à **512 Mio** ; si cette limite est dépassée, un message demande
une sélection plus petite. Aucun lot incomplet n’est partagé. Les limites existantes
par rapport restent applicables (160 Mio par ZIP, certaines données pouvant être
omises ou tronquées et signalées dans le manifeste). L’export groupé ne permet pas
de récupérer des données que la collecte individuelle n’a pas conservées.
Si l’activité est détruite pendant l’export, celui-ci est annulé au prochain point
de contrôle, au plus tard après la génération du rapport individuel en cours.

## Vérifications effectuées

Tests JVM compilés avec Kotlin 1.9.22 et Java 17, sur le véritable exporteur,
le code de regroupement, les fenêtres de dates et le verrouillage de fichiers :

- Deux rapports distincts, ordre de sélection, dédoublonnage et manifestes exacts.
- Journaux sources conservés, suppression des ZIP individuels temporaires.
- Format de l’export individuel inchangé ; session active identifiée.
- Sélection vide, session invalide après un premier rapport valide : aucun ZIP partiel conservé.
- Dépassement du budget et interruption pendant la copie : nettoyage vérifié.
- Bornes de jours/semaines, changement d’année, jours de 23 et 25 heures.

Commande :

```
python3 tools/diagnostics_tests/export_batch/run.py --dependencies /chemin/vers/jars
```

Les dépendances sont documentées dans ce script. Les seules doublures concernent
le contexte Android, la racine des sessions et les petits utilitaires date/JSON.
Les ressources XML anglais/français et la cohérence des clés ont aussi été vérifiées.

**Limites :** aucun APK construit et aucune interaction sur téléphone exécutée ici.
À vérifier sur Android : cases à cocher, sélection après rotation, export par le
sélecteur de partage, ouverture du lot reçu, export individuel et message de limite.

## Retour sur les corrections 3D

Le retour utilisateur indique peu ou pas d’amélioration visible avec la précédente
livraison. Les problèmes de rendu et de cinématique restent donc non résolus sur
l’appareil. Cette livraison ajoute uniquement l’export groupé : elle ne contient
aucune nouvelle correction graphique ni promesse de performance.
Un lot contenant les sessions affectées, avec le jeu et le symptôme associés,
permettra de poursuivre l’analyse à partir des erreurs réellement observées.
