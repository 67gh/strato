# Correctifs 3D et vidéo — 28 octobre 2026

Ces modifications corrigent des défauts identifiés dans les sources fournies.
Elles visent le rendu utilisé par TOTK et Sparking Zero, et le parcours vidéo
utilisé par des jeux comme Hollow Knight. Aucun de ces trois jeux n'a été
exécuté ici : leur réparation complète et leurs performances restent à confirmer.

## Base conservée

- Archive fournie : `strato.zip`.
- SHA-256 de l'archive d'entrée : `0bde12ab1aa4a8d44d25fcac1b2dea07257367bebb0c11b143d467d7df34944b`.
- HEAD inclus : `0b4c3badd498ad4d6322e2125a5ab83f0e3fb165`.
- Tous les fichiers d'origine sont conservés, avec remplacement des seuls
  fichiers listés dans le manifeste et ajout des tests/du présent bilan.
- Les changements préexistants du compilateur de shaders et de Vulkan-Hpp
  sont conservés. Aucun commit distant ni publication GitHub n'a été effectué.
- Le décodage H.264/VP8, VIC monocouche et le correctif SMMU préexistant restent
  présents. Cette intervention n'ajoute pas VP9 ni le désentrelacement.

## Modifications

1. **Recherche des mipmaps.** La boucle de recherche d'une vue dans une texture
   n'avançait son offset et son numéro de mip que lorsque l'offset correspondait
   déjà. Les niveaux suivants pouvaient donc être manqués. Les compteurs avancent
   maintenant à chaque niveau. Les correspondances couche/mip sont testées.
2. **Dimensions des mipmaps.** Les dimensions en texels sont divisées avant
   d'arrondir en blocs mémoire. L'ancien calcul divisait des tailles déjà
   remplies de padding, ce qui faussait les offsets de certaines textures non
   puissances de deux. Les chemins de calcul de taille et de chargement utilisent
   maintenant le même calcul corrigé, y compris pour les textures compressées/3D.
3. **Écritures de shaders dans les images.** Elles participent maintenant au
   suivi des producteurs GPU, aux dépendances lecture/écriture et à la politique
   de récupération des données par le processeur. Auparavant, cette dernière
   reposait seulement sur l'utilisation comme cible de rendu.
4. **Ordre des passes.** Une dépendance entre deux dessins force la séparation
   des passes : la barrière ne doit pas être déplacée avant le dessin qui produit
   les données. Les cibles de rendu attendent aussi les accès des shaders de calcul.
5. **Vues d'images utilisées en écriture.** Elles utilisent un seul mip et des
   canaux sans permutation, conformément aux contraintes des storage images
   Vulkan. La vue d'échantillonnage reste intacte. Une transition de layout déjà
   satisfaite n'impose plus une attente inutile du GPU.
6. **VIC et transparence.** Le mode alpha 0 force désormais l'opacité au lieu
   de recopier l'alpha de la source. Les modes 1 (alpha du fond) et 2 (alpha de
   destination conservé) sont pris en charge. Cela supprime un défaut pouvant
   rendre une image vidéo transparente, sans prouver que c'est la cause exacte
   du problème signalé dans Hollow Knight.
7. **VIC et mises à jour partielles.** Le rectangle de sortie peut couvrir une
   partie de l'image. Les pixels et le padding en dehors sont préservés. Pour
   NV12/NV21, le rectangle doit couvrir des groupes chroma complets ou le bord
   extérieur de l'image ; les cas non pris en charge restent explicitement rejetés.

## Vérifications réalisées

Tests C++ hôte avec AddressSanitizer et UndefinedBehaviorSanitizer, selon les
scripts concernés. Les méthodes GPU sont compilées avec des dépendances de
test et les vrais types Vulkan ; elles ne s'exécutent pas sur un GPU Vulkan.

| Vérification | Résultat |
|---|---|
| 1 080 chaînes de mipmaps, avec oracle indépendant de taille | Réussi |
| Recherche des vues de toutes les couches/mips et rejets d'offsets invalides | Réussi |
| Dépendances shader-image, écritures répétées, plusieurs producteurs, reset | Réussi |
| Binding réel des storage images, mip unique et canaux identité | Réussi |
| Condition réelle de séparation des passes | Réussi |
| 120 cas de décodeur/encodeur BC | Réussi |
| 448 cas d'intégration BC + swizzle + chargement/lecture de texture | Réussi |
| 192 cas VIC plein écran, 192 partiels, 384 alpha, 4 NV12/NV21 partiels | Réussi |
| SMMU : plage utilisée dans l'erreur Hollow Knight et bornes d'adressage | Réussi |
| Sécurité des commandes Host1x | Réussi |
| Anciens calcul/recherche des mips avec les nouveaux tests | Échec attendu : régressions reproduites |
| Ancien VIC avec le test d'opacité | Échec attendu : défaut reproduit |
| `git diff --check` | Réussi |

Le test d'intégration BC a aussi été réparé : son enum Vulkan Float/Sfloat
était obsolète, et son pitch de test devait être adapté aux nouvelles grandes
dimensions testées. La production possédait déjà le bon enum Sfloat.

## Limites et risques restants

- **Pas d'APK construit.** Le wrapper Gradle échoue au téléchargement de Gradle
  8.2 ; SDK/NDK Android absents et plusieurs dépendances natives non matérialisées.
  Les tests hôte ne remplacent pas une compilation Android complète.
- La suite complète de décodage H.264/VP8 n'a pas été relancée avec succès :
  téléchargement de FFmpeg 7.1.2 bloqué (HTTP 503), archive de secours incomplète.
  Le décodeur n'a pas été modifié. Les résultats historiques de 24 images ne sont
  pas présentés comme une nouvelle validation.
- Les nouveaux découpages de passes privilégient la cohérence des données et
  peuvent augmenter le coût GPU. Aucun gain de FPS n'a été mesuré.
- Pas de capture GPU ni de cinématique Hollow Knight permettant de reproduire
  le symptôme exact. D'autres causes restent possibles : shaders, alias de
  textures, pilotes, modes VIC/codecs non pris en charge ou problèmes CPU.

## Reproduire les tests

Depuis la racine du projet, avec Python 3 et g++ C++20 :

```sh
python tools/render_tests/image_coherence.py
python tools/render_tests/storage_binding.py
python tests/run_bc.py
python tests/run_texture.py
python tools/smmu_tests/run.py
python tools/render_tests/host1x_safety.py
```

Le test VIC peut être exécuté séparément de FFmpeg :

```sh
g++ -std=c++20 -Wall -Wextra -Wconversion -Werror -g \
  -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie \
  -Iapp/src/main/cpp/skyline/soc/host1x/video \
  tools/video_tests/compositor_test.cpp \
  app/src/main/cpp/skyline/soc/host1x/video/compositor.cpp -o /tmp/strato-vic-test
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 /tmp/strato-vic-test
```

Pour la validation appareil : utiliser la même version du jeu, le même pilote,
la même sauvegarde et la même résolution avant/après. Vérifier une scène 3D et
les changements de zone dans TOTK/Sparking Zero ; dans Hollow Knight, vérifier
la cinématique du début à la fin, le son et le retour au jeu. Conserver les logs
et les captures si un défaut persiste. Ne pas conclure à une réparation sur la
seule base d'une capture correcte ou d'un démarrage réussi.

## Références étudiées

- [BOTWRecompAndroid, calcul Tegra](https://github.com/mamcjr/BOTWRecompAndroid/blob/main/runtime/nvn/tegra.c), blob `824523dc5ab0fc4bb7397dad699c76d85ac03d7a`.
- [BOTWRecompAndroid, exécution Vulkan](https://github.com/mamcjr/BOTWRecompAndroid/blob/main/runtime/nvn/vulkan/exec.c), blob `f8143124bdf5a441eead44dd4da76a131b4ce207` : cibles persistantes et attente des pipelines. Strato attendait déjà ses pipelines ; ce mécanisme n'a pas été recopié.
- [Spécification Vulkan : synchronisation](https://docs.vulkan.org/spec/latest/chapters/synchronization.html).
- [Documentation NVIDIA VIC](https://github.com/NVIDIA/open-gpu-doc/blob/master/classes/video/vic_ceb6_types.h), blob `60f1b58d97c9d1920c750a6554fbcd752b6f073a` : sémantique du rectangle cible et des modes alpha. Le layout ABI existant de Strato est conservé.

Les corrections ont été écrites pour l'architecture de Strato. Il n'y a pas
d'intégration du moteur de BOTWRecompAndroid ni d'import de code du jeu.
