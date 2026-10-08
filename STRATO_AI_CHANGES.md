# Strato — Journal des modifications IA

Ce fichier conserve l'historique des modifications apportées au projet pendant les sessions de correction.

> Règle de travail : lors des prochaines corrections, seuls les fichiers réellement modifiés seront livrés, avec leur chemin d'origine. Ce journal sera mis à jour en même temps.

## 2026-10-04 — GPU / Vulkan + intégration Dynarmic

### `app/CMakeLists.txt`
- Renforcement de l'intégration Dynarmic A64.
- Désactivation des PCH Dynarmic pour éviter des problèmes de cross-compilation avec le NDK Android.
- Vérification de l'existence de la cible CMake `dynarmic` après `add_subdirectory`.
- Désactivation propre du fallback JIT si la cible Dynarmic n'est finalement pas créée.

### `app/build.gradle`
- Activation explicite de `STRATO_JIT_FALLBACK=ON` pour le build `debug`.
- Les configurations `release` / `reldebug` avaient déjà été prises en compte dans les modifications précédentes de cette intégration.

### `app/src/main/cpp/skyline/nce/jit_fallback.cpp`
- Correction des lectures mémoire génériques : remplacement des déréférencements par `reinterpret_cast` par `memcpy` afin d'éviter les problèmes d'alignement et d'aliasing.
- Correction des écritures mémoire génériques avec `memcpy`.
- Retour immédiat après une écriture mémoire invalide.
- Ajout d'un contrôle d'alignement pour les écritures exclusives atomiques.
- Correction de `MemoryReadCode()` pour utiliser `memcpy`.
- Correction du traitement de `Dynarmic::A64::Jit::Disassemble()` : l'API embarquée renvoie un `vector<string>` et non une chaîne possédant `substr()`.
- Limitation du dump de désassemblage à 4096 octets.
- Réinitialisation de l'état `seen` lors d'une nouvelle initialisation du fallback.
- Fermeture de l'ancien descripteur de log avant d'en ouvrir un nouveau.

### `app/src/main/cpp/skyline/gpu.cpp`
- Protection contre les pointeurs Vulkan `messageCStr` nuls dans les messages de validation/debug.
- Protection contre `layerPrefix` nul dans les diagnostics Vulkan.
- Chargement de `libvulkan.so` avec `RTLD_LOCAL`.
- Ajout d'une erreur explicite lorsque `libvulkan.so` ne peut pas être chargé.
- Vérification explicite de la présence/export de `vkGetInstanceProcAddr`.

### `app/src/main/cpp/skyline/gpu/diagnostics.cpp`
- Réinitialisation de `events` lors de l'initialisation des diagnostics.
- Réinitialisation de `reported` afin qu'une nouvelle instance GPU puisse produire son propre rapport de faute.

## Vérifications effectuées

- Configuration CMake de Dynarmic/A64 : **OK**.
- Présence et utilisation de l'API `Disassemble()` de la version Dynarmic embarquée : **corrigée**.
- Compilation ciblée de Dynarmic : progression sans erreur de compilation jusqu'à environ 61 % avant interruption par la limite de temps de l'environnement.
- Build Android complet : non validé dans l'environnement de travail, car les dépendances/sous-modules nécessaires ne sont pas tous disponibles localement et l'environnement n'a pas accès au réseau pour les récupérer.

## État actuel

### Dynarmic
- Intégration CMake : corrigée.
- Backend A64 : configuré.
- Fallback JIT : raccordé au build Android debug et protégé contre l'absence de cible Dynarmic.
- Accès mémoire : corrigés pour les cas non alignés/non sûrs.
- Dump de désassemblage : corrigé pour l'API Dynarmic actuellement embarquée.

### GPU / Vulkan
- Diagnostics Vulkan : renforcés.
- Chargement dynamique de Vulkan : erreurs explicites.
- État des diagnostics : correctement réinitialisé entre deux initialisations GPU.

## À vérifier lors du prochain build local

1. `./gradlew assembleDebug`
2. Vérifier que `STRATO_JIT_FALLBACK=ON` apparaît dans la configuration CMake du build debug.
3. Vérifier que la cible `dynarmic` est bien construite par CMake.
4. Tester le démarrage Vulkan sur l'appareil Android.
5. Tester un titre nécessitant le fallback Dynarmic.
6. Si une erreur apparaît, ajouter le correctif au présent journal avec le fichier exact modifié.

## 2026-10-05 — Correction GitHub Actions / sous-modules

### `.github/workflows/build.yml`
- Ajout d'une étape explicite d'initialisation des sous-modules après le checkout.
- Exécution de `git submodule sync --recursive`.
- Exécution de `git submodule update --init --recursive --force`.
- Affichage de `git submodule status --recursive` pour rendre l'état des sous-modules visible dans les logs CI.

### `.github/workflows/pr_build.yml`
- Même initialisation explicite des sous-modules pour les builds de Pull Request.

### Problème corrigé
- Le checkout utilisait déjà `submodules: recursive`, mais le build GitHub fourni montrait que les répertoires `app/libraries/*` requis par CMake étaient absents.
- Le correctif force donc la synchronisation et l'initialisation des sous-modules avant toute étape CMake/Gradle.
- Aucun dépôt personnel n'est créé : les URLs déjà présentes dans `.gitmodules` restent utilisées.

### Vérifications
- Vérification statique des deux workflows : étape placée immédiatement après le checkout.
- Le build Android GitHub n'est pas exécuté dans cet environnement ; son succès doit être confirmé par une nouvelle exécution GitHub Actions.

## 2026-10-07 — Phase 1 : socle de diagnostic exportable et traces NCE fiables

Première livraison de code après audit de l'archive fournie. La refonte complète
reste à poursuivre par phases ; aucun gel de TotK ni DEVICE_LOST de Dragon Ball
n'est déclaré résolu. Procédure, validations et limites : `docs/DIAGNOSTIC_PHASE1.txt`.

### Diagnostic Android / JNI

- Ajout de `DiagnosticSession.kt`, `DiagnosticExporter.kt`, `DiagnosticActivity.kt`,
  layout et chaînes FR/EN. Sessions persistantes distinctes, collecte appareil/
  réglages/CPU/RSS/threads hôtes/cadence des présentations réelles et logcat du PID.
- Export ZIP plafonné avec manifeste d'intégrité structurelle et de disponibilité ;
  les fichiers manquants, tronqués et captures actives sont explicitement signalés.
- Sélection et suppression explicite des anciennes sessions ; verrou actif et
  traversées de répertoires/symlinks refusés lors de la suppression.
- Intégration dans `EmulationActivity.kt`, `StratoApplication.kt`, `MainActivity.kt`,
  `SettingsHomeFragment.kt`, manifeste et FileProvider limité au cache diagnostics.
- `emu_jni.cpp` : chemin de session, état JIT compilé, exceptions JNI, fin des traces
  NCE après arrêt des threads. Aucun changement de signature publique hors liaison
  Kotlin/JNI interne, modifiée des deux côtés.
- `common/diagnostic_session.h` : publication atomique des JSON natifs ;
  `common/diagnostic_metrics.h` et `presentation_engine.cpp` : compteur atomique des
  présentations réelles, sans log par image et sans compter les images interpolées.

### Vulkan / garde-fous

- `gpu.cpp` et `gpu/diagnostic_snapshot.h` : snapshot d'initialisation du pilote,
  extensions annoncées/activées, traits/quirks, heaps/types et budget éventuel.
  Le budget du physical device n'exige pas l'activation de l'extension sur le device.
- `gpu/diagnostics.cpp/.h` : fichier par session, événements bornés et horodatés,
  initialisation réinitialisée, erreurs de diagnostic non propagées.
- `os.cpp` : routage du journal NCE et arrêt watchdog sur exception d'exécution.
- `loader/loader.cpp/.h` : vérification de fin/plage avant résolution de symboles,
  copie des plages de modules avant démarrage des threads pour les traces NCE.

### NCE / analyse hors ligne

- `nce.cpp` : correction `mrs.srcReg` vers `msr.srcReg` dans le choix du registre
  temporaire du patch MSR TLS.
- `nce/jit_fallback.cpp/.h` : schéma v2 avec état avant JIT conservé, résultat séparé,
  horodatages, threads, FPCR/FPSR, module+offset ; lecture opcode via `/proc/self/mem`,
  compteurs cumulés dédoublonnés bornés et clôture non levante par `Flush()`.
- L'indisponibilité de `/proc/self/mem` refuse désormais explicitement le fallback
  SIGILL. La sûreté des accès données JIT n'est PAS démontrée par le `memcpy` mentionné
  dans l'historique du 4 octobre ; droits/pages/rollback restent à traiter.
- `tools/analyze_nce_fallback.py` : fréquences, ancien/nouveau schéma, données
  tronquées et suggestions manuelles. Deux fichiers de tests dans `tools/tests/`.

### Validation et travail restant

- 21 tests Python réussis, dont quatre vérifications des formats JSON C++ extraits.
- XML et parité FR/EN vérifiés ; revue croisée des changements. Le patch et ses
  fichiers sont accompagnés d'un rapport de vérification dans la livraison.
- Build Android, installation, performances et jeux : **non exécutés ici**.
- Aucun ajout de dépendance Android ni modification de CMake/Gradle/workflows.
- Axe B restant : watchdog étendu, threads/SVC/attentes, IPC/stubs, compteurs GPU.
- Axe D restant : vrais descripteurs storage/texel, device_fault, SPIR-V et couleurs.
- Axe C restant : mode forcé, sémantiques natives, oracle et accès mémoire JIT.
- Axe A restant : refonte globale bibliothèque/paramètres/overlay.
