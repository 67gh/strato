# Strato — Journal et passation pour IA

Ce fichier est fait pour être donné à une IA (Claude, ChatGPT…) au début d'une session : il dit où en est le projet,
ce qui a été modifié et pourquoi, ce qui reste à faire, et comment le faire. **Mets-le à jour à chaque livraison.**

Dernière mise à jour : 2026-10-04.

---

## 0. Règles de travail (à respecter)

1. **Langue** : répondre en français, court et direct (l'utilisateur lit sur téléphone et dicte ses messages, il y a des fautes de dictée).
2. **Livraisons** : ne livrer que les fichiers réellement modifiés, avec leur chemin d'origine (zip) + un `.patch` (`diff -ruN a b`), vérifié avec `patch -p1 --dry-run`.
   Ne jamais renvoyer des fichiers déjà livrés et bons : ils écraseraient des corrections faites ailleurs (c'est déjà arrivé avec ChatGPT).
3. **Ce fichier** doit être inclus à la racine dans chaque livraison, mis à jour (nouvelle section datée + état des problèmes).
4. **Honnêteté** : l'IA n'a ni NDK, ni Android, ni réseau dans son environnement. Rien de ce qui est listé ci-dessous n'a été compilé ou testé par l'IA,
   sauf mention contraire. Toujours dire ce qui est vérifié et ce qui ne l'est pas. Les valeurs de protocole écrites de mémoire sont marquées « à vérifier ».
5. **Légal** : ne pas embarquer de fichiers Nintendo (firmware, polices officielles, clés) dans l'APK. Le code d'autres émulateurs peut avoir une autre licence
   (voir la licence du dépôt ; yuzu = GPL-3.0) : s'en inspirer, ne pas copier sans vérifier.
6. Avant de modifier un fichier, demander/regarder sa version actuelle chez l'utilisateur si une autre IA a pu le toucher (voir section 6).

## 1. Contexte

- Projet : **Strato**, un fork de Skyline (émulateur Switch pour Android). Package `org.stratoemu.strato`. Les fichiers portent `SPDX-License-Identifier: MPL-2.0` (vérifier le fichier LICENSE du dépôt pour la licence du projet).
- Appareil de test : Samsung S21 (Snapdragon 888, Adreno 660), pilote Vulkan Turnip (Mesa, freedreno/KGSL).
- Build : GitHub Actions (NDK 26.1.10909125, CMake 3.31.5, Ninja), sous-modules dans `app/libraries/*`. Pas de build local.
- Jeux testés : **Zelda Tears of the Kingdom** (0100F2C0115B6000), **Dragon Ball Sparking! ZERO** (010035F022078000), **Hollow Knight**.
- Le NCE de Strato n'est PAS un système « bloc par bloc » : le code du jeu s'exécute directement sur le CPU. `NCE::PatchCode` ne remplace au chargement que les `SVC`
  et certains `MRS/MSR` (TPIDR, compteurs). Il n'y a pas de « blocs que NCE n'a pas réussi » : ce qui peut échouer, c'est une instruction refusée par le CPU (SIGILL),
  un accès mémoire invalide (SIGSEGV) ou un SVC non géré.

### Fichiers produits sur l'appareil
Dans `/storage/emulated/0/Android/data/org.stratoemu.strato/files/` :
- `emulation.log` (copié par l'utilisateur, niveau réglable dans Paramètres > Déboguer ; **Debug** montre les commandes IPC)
- `nce_fallback.jsonl` : une ligne `session` par lancement (`jit_compiled` true/false) et une ligne `failure` par instruction qui a fait planter un thread invité
- `gpu_fault.log` : rapport des ~400 derniers événements GPU quand le pilote renvoie `VK_ERROR_DEVICE_LOST`
- `crash.log` : exception Java/Kotlin non gérée (ajouté pour diagnostiquer sans adb)
- `firmware_lite.zip` : écrit après chaque import de firmware (version système + polices seulement)

## 2. Comment lire les logs

- `NCE fallback: Dynarmic JIT ENABLED / NOT compiled in` (ligne au démarrage du jeu) : dit si le JIT est dans le build.
- `[STUB] <Service>::GetServiceFunction not implemented (cmdId=0x.., tipc=..)` : un jeu a appelé une commande IPC absente. Le service est nommé. Strato renvoie `0xF601`.
  Si juste après on voit `SetTerminateResult ... 62977` (= 0xF601 = erreur 2001-0123), le jeu s'est fermé tout seul à cause de cette commande manquante.
- `Thread #N has crashed due to signal` : plantage CPU invité (la trace donne PC et module).
- `GPU faulted or hung (VK_ERROR_DEVICE_LOST)` : plantage du GPU/pilote (pas un problème CPU).
- `VK_ERROR_OUT_OF_POOL_MEMORY` : **bénin**, `DescriptorAllocator` crée un nouveau pool.
- `MakePipelineDescriptorInfo: Image (buffer) descriptors are not supported` : un shader utilise des images de stockage/texel buffers que Strato ne gère pas.
- `Skipping compute dispatches of pipeline ...` : contournement actif (section 4.4).

### Recette pour implémenter une commande IPC manquante
1. Dans `Classe.h` : déclarer `Result Fonction(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);` et l'ajouter dans `SERVICE_DECL(SFUNC(0xID, Classe, Fonction), ...)`.
2. Dans `Classe.cpp` : lire les entrées avec `request.Pop<T>()`, écrire les sorties avec `response.Push<T>()`, `return {};` pour succès.
3. Référence des commandes : switchbrew.org (Filesystem services, Applet Manager services, HID services, Audio services). Marquer « à vérifier » ce qui vient de la mémoire.

## 3. Historique des modifications (par thème)

### 3.1 Services HOS (pour que les jeux dépassent le démarrage)
| Fichier | Changement | Pourquoi |
|---|---|---|
| `services/base_service.h` (ligne ~129) | le message `[STUB]` affiche `GetName()` | savoir quel service manque. **Appliqué à la main par l'utilisateur, pas dans les zips de l'IA** |
| `services/fssrv/ISaveDataInfoReader.h/.cpp` | cmd `0x0 Read` : renvoie 0 entrée | TotK s'arrêtait là (aucune sauvegarde listée) |
| `services/am/controller/IApplicationFunctions.h/.cpp` | cmd `0x1B CreateCacheStorage` : renvoie `u32 2` (SdCard) + `u64 0` | TotK. **Format de sortie à vérifier** |
| `services/hid/IHidServer.h/.cpp` | cmd `0x6B DisconnectNpad` : ne fait rien | TotK l'appelle au démarrage |
| `services/codec/IHardwareOpusDecoderManager.h/.cpp`, `IHardwareOpusDecoder.h/.cpp` | cmds `0x2/0x3/0x6/0x7` (décodeur Opus multi-flux, normal et « Ex »), `MultiStreamParameters(Ex)` | Dragon Ball utilise l'audio 6 canaux. **Layout des paramètres Ex (0x118) de mémoire** |

### 3.2 JIT de secours (Dynarmic) et journal d'échecs NCE
- `nce/jit_fallback.h/.cpp` (nouveaux) : `JitFallback::HandleFault`. Sur SIGILL, exécute **une seule instruction** avec Dynarmic (A64) sur la pile hôte (`hostSp`), recopie les registres dans le `ucontext`, reprend en natif.
  Chaque échec distinct est écrit dans `nce_fallback.jsonl` : `pc`, `insn`, `decoded` (nom de l'instruction / registre système), `class`, `signal`, `outcome`
  (`recovered_by_jit`, `jit_unsupported`, `jit_memory_fault`, `not_attempted`), `where` (module+offset), et `jit` = registres modifiés, accès mémoire, `next_pc`.
  Le but : savoir quelles instructions il faut gérer nativement dans `NCE::PatchCode` (il ne faut PAS réutiliser le code machine de Dynarmic, il dépend de son état interne ; il faut réécrire la sémantique).
- `nce.cpp` : `SignalHandler` appelle `JitFallback::HandleFault` avant de traiter le plantage. `os.cpp` : `JitFallback::Initialize(state, publicAppFilesPath + "nce_fallback.jsonl", nom du jeu)`.
- Build : `.gitmodules` (sous-module `app/libraries/dynarmic` → `https://github.com/lioncash/dynarmic`, car `merryhime/dynarmic` a été supprimé), option CMake `STRATO_JIT_FALLBACK` (ON par défaut, désactivée proprement si Dynarmic est absent),
  contournement Boost (voir 3.7), `build.gradle` passe `-DSTRATO_JIT_FALLBACK=ON` (release, reldebug, debug). Option facultative `STRATO_JIT_DUMP_DISASSEMBLY`.
- **Autre IA (ChatGPT)** : mémoire via `memcpy`, contrôle d'alignement des écritures exclusives, `Disassemble()` renvoie un `vector<string>` dans la version embarquée de Dynarmic (corrigé), réinitialisation de `seen`/fd à chaque `Initialize`,
  PCH Dynarmic désactivés, vérification que la cible `dynarmic` existe après `add_subdirectory`, protections dans `gpu.cpp` (messages Vulkan nuls, `dlopen` avec erreurs explicites), `diagnostics.cpp` réinitialisé à chaque init.

### 3.3 Diagnostics GPU
- `gpu/diagnostics.h/.cpp` (nouveaux) : tampon circulaire de 400 événements, `DumpFaultReport` écrit `gpu_fault.log` (une fois par lancement). Branché dans `gpu.cpp` (callback debug Vulkan), `gpu/fence_cycle.h`, `gpu/command_scheduler.cpp`.
- `gpu/interconnect/maxwell_3d/pipeline_manager.h/.cpp` : `DescribeUnwrittenDescriptors()`, événements « created » et résumé du chargement du cache. `maxwell_3d.cpp` : événement `bind` à chaque changement de pipeline.
  **Limite** : cela ne couvre que les pipelines graphiques ; les avertissements « Image descriptors… » viennent des pipelines de calcul (Kepler compute).

### 3.4 Contournement GPU : shaders de calcul (À TESTER)
- `gpu/interconnect/kepler_compute/pipeline_manager.h/.cpp`, `kepler_compute.cpp` : un pipeline de calcul dont le shader déclare des images de stockage ou des texel buffers (jamais écrits dans le descriptor set) est **ignoré** au `Dispatch`
  (`HasUnsupportedDescriptors()`), avec un log unique `Skipping compute dispatches...`. Hypothèse : c'est ce qui fait planter l'Adreno (`DEVICE_LOST`) dans Dragon Ball. Effet secondaire : l'effet visuel du shader manque.

### 3.5 Interface
- **Paramètres en liste de catégories** (comme la capture de référence) : `settings/SettingsHomeFragment.kt`, `res/layout/fragment_settings_home.xml`, `settings_home_item.xml`, icônes `res/drawable/ic_settings_*.xml`,
  chaînes `values/strings.xml` + `values-fr/strings.xml`. Chaque catégorie ouvre **une `SettingsActivity` propre** (`EXTRA_TITLE`, `EXTRA_CATEGORIES`) qui affiche un `GlobalSettingsFragment` filtré (`newInstance`).
  `AndroidManifest.xml` : `launchMode="singleTop"` retiré de `SettingsActivity`. « Par défaut » efface les clés des préférences d'émulation. Pas de catégorie « Caméra » (rien à régler).
  Historique : une première version (transaction de fragments + pile de retour) plantait au clic ; remplacée par une activité par catégorie. **L'utilisateur n'a pas encore confirmé que ça marche.**
- `StratoApplication.kt` : `UncaughtExceptionHandler` qui écrit `crash.log`.
- **Plusieurs dossiers de jeux** : `AppSettings.searchLocationList` (clé `search_locations`, un URI par ligne ; `search_location` reste la compatibilité), `RomProvider.loadRoms(List<Uri>)` (dossier illisible ignoré, doublons retirés),
  `MainViewModel`, `MainActivity`, `preference/SearchLocationsPreference.kt`, `res/xml/app_preferences.xml`.
- **Écran de premier lancement** : `SetupActivity.kt`, `res/layout/setup_activity.xml`, `setup_step.xml`, `AppSettings.setupCompleted`, manifeste (`POST_NOTIFICATIONS`). Étapes : notifications, dossiers de jeux (SAF = autorisation de stockage), `prod.keys`, `title.keys`, firmware.
  N'apparaît que si aucun dossier n'est configuré et que `setupCompleted` est faux.

### 3.6 Clés et firmware
- `crypto/key_store.h` : `IndexedKeys128` passe de 20 à **32** générations (le firmware 21/22 dépasse 0x13). `vfs/nca.cpp` : vérifications de limites sur `titleKek[keyGeneration]` et `keys[keyGeneration]` (avant : lecture hors tableau).
- `KeyReader.kt` : un `prod.keys` n'est plus rejeté en entier à la première ligne inhabituelle (commentaires, lignes sans `=`).
- `preference/FirmwareImportPreference.kt` : `importFirmware(uri)` réutilisable (aussi par `SetupActivity`), accepte les zips avec sous-dossiers/fichiers non-NCA, valide si version système **ou** polices présentes (« Lite »),
  écrit `firmware_lite.zip`. `loader_jni.cpp` : `listEssentialArchives` (NCAs `0100000000000809` et `…810` à `…814`).
  Note : Strato embarque déjà des polices de remplacement libres (`assets/fonts`), le firmware n'est donc PAS indispensable ; il ne sert qu'à avoir les vraies polices Nintendo. Les jeux peuvent demander d'autres archives système (Mii `…802`, fuseaux `…80E`, certificats `…800`) via `OpenDataStorageByDataId`.

### 3.7 Build : Boost pour Dynarmic
`find_package(Boost 1.57 REQUIRED)` de Dynarmic échouait (Boost est un super-projet CMake sans dossier d'en-têtes unique). `app/CMakeLists.txt` : `find_path(boost/version.hpp)` dans `libraries/boost/libs/config/include`, `Boost_INCLUDE_DIR` forcé,
`include_directories(SYSTEM libraries/boost/libs/*/include)`, désactivation du JIT avec un avertissement si introuvable. L'utilisateur indique avoir aussi résolu le problème en installant la bibliothèque Boost de son côté.

## 4. État des problèmes (au 2026-10-04)

### 4.1 JIT / NCE
- Aucun SIGILL n'a jamais été enregistré (39 sessions, 0 ligne `failure`). Sur un Snapdragon 888 c'est normal : le CPU est plus récent que celui de la Switch. Le JIT n'a donc **rien prouvé** pour l'instant.
- Les derniers logs disent encore `NOT compiled in` : le build testé n'avait pas le JIT. **À faire** : lire dans la sortie CMake la ligne `-- Dynarmic JIT fallback: ON/OFF` et les `WARNING` voisins.
  Ne pas attribuer à Dynarmic le fait que Zelda démarre : les correctifs de services (3.1) l'expliquent.

### 4.2 Zelda: Tears of the Kingdom
- Chaîne résolue : `ISaveDataInfoReader` → `CreateCacheStorage` → `DisconnectNpad`. Le jeu atteint le menu, charge la sauvegarde (sons OK), puis **gèle sur l'écran de chargement** (pas d'erreur de service, pas de plantage CPU ni GPU dans les logs).
- **À faire** : log en niveau Debug pendant le gel (dernières commandes IPC, services attendus). Idées : watchdog qui, après N secondes sans activité, journalise l'état des threads invités (SVC bloquant, PC + module), et vérifier les accès fichiers de la sauvegarde.

### 4.3 Dragon Ball Sparking! ZERO
- L'audio multi-canaux passe. Le jeu atteint l'écran titre/menu (≈ 30 fps, une vingtaine de pipelines), puis `VK_ERROR_DEVICE_LOST` à des moments variables (12 à 63 s), toujours après des pipelines de **calcul** avec `Image descriptors are not supported`. Changer de pilote GPU n'a rien changé.
- Le correctif 3.4 est livré mais **pas encore testé**. Bugs de couleur au lancement signalés, **non analysés** (il faut une capture d'écran).

### 4.4 Vraie correction GPU (à faire)
Implémenter les images de stockage (`eStorageImage`, `ImageView` avec usage storage, layout GENERAL) et les texel buffers (`eUniformTexelBuffer`, `eStorageTexelBuffer`) dans
`kepler_compute/pipeline_manager.cpp` (`SyncDescriptors`) et `maxwell_3d/pipeline_manager.cpp`. Aujourd'hui `MakePipelineDescriptorInfo` crée les emplacements dans le layout mais n'écrit jamais les descripteurs.
Piste de secours : activer `nullDescriptor` (VK_EXT_robustness2) pour que les emplacements vides soient lisibles. Skyline amont est abandonné et aucun fork trouvé n'implémente ceci : à écrire à partir du compilateur de shaders (yuzu) qui sait déjà émettre ces accès.

### 4.5 Autres
- Hollow Knight : s'arrête sans erreur dans les logs (non analysé plus loin).
- Import du firmware 22.5.0 + `prod.keys` récent : correctifs 3.6 livrés, **non testés**.
- Premier lancement / dossiers multiples / paramètres : livrés, **non confirmés** par l'utilisateur.

## 5. À faire, dans l'ordre conseillé
1. Faire confirmer/infirmer : paramètres (clic sur une catégorie), écran de premier lancement, dossiers multiples.
2. Tester le correctif de calcul (3.4) avec Dragon Ball ; envoyer `emulation.log` + `gpu_fault.log`.
3. Zelda : log Debug pendant le gel de l'écran de chargement.
4. Vérifier pourquoi le JIT n'est pas compilé (sortie CMake) ; puis seulement chercher un jeu qui produit un SIGILL.
5. Si la compilation échoue : demander la **première** ligne `error:` ou `CMake Error`, corriger uniquement le fichier concerné.
6. GPU : implémenter les images de stockage / texel buffers (4.4) ; enquêter sur les couleurs de Dragon Ball.
7. Commandes 8 et 9 d'`IHardwareOpusDecoderManager` (firmware récent) si un jeu les réclame.

## 6. Fichiers que plusieurs IA ont touchés (ne pas écraser)
`app/CMakeLists.txt`, `app/build.gradle`, `nce/jit_fallback.h/.cpp`, `gpu.cpp`, `gpu/diagnostics.cpp`. La version de référence est **celle de l'utilisateur** (adoptée à partir des fichiers qu'il a envoyés le 2026-10-04 pour `gpu.cpp`, `CMakeLists.txt`, `build.gradle`, `jit_fallback.h/.cpp` ;
`diagnostics.cpp` modifié par ChatGPT n'a pas été revu). Demander la version actuelle avant de les modifier.

## 7. Historique des livraisons de l'IA (Claude)
Les paquets livrés (zips + patchs `strato-jit-fallback`, `strato-modified-files`) cumulaient tout jusqu'à 57 fichiers ; le dernier paquet autonome est **`strato-compute-fix`** (3 fichiers `kepler_compute`) + ce journal.
À partir de maintenant : un paquet = uniquement les fichiers modifiés depuis la dernière livraison + ce journal mis à jour.
