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

## 2026-10-05 — Implémentation storage images Vulkan

### Fichiers modifiés
- `app/src/main/cpp/skyline/gpu/interconnect/common/pipeline.inc`
- `app/src/main/cpp/skyline/gpu/interconnect/kepler_compute/pipeline_manager.cpp`
- `app/src/main/cpp/skyline/gpu/interconnect/maxwell_3d/pipeline_manager.cpp`
- `app/src/main/cpp/skyline/gpu/texture/texture.cpp`

### Changement
- Ajout d'un chemin commun pour construire les descripteurs `VK_DESCRIPTOR_TYPE_STORAGE_IMAGE` sans sampler.
- `kepler_compute` écrit désormais les `image_descriptors` dans `SyncDescriptors`.
- `maxwell_3d` écrit désormais les `storageImageDescs` dans `SyncDescriptors`.
- Les storage images sont utilisées en layout `VK_IMAGE_LAYOUT_GENERAL` et marquées GPU-dirty lorsqu'elles sont écrites.
- Les textures dont le format hôte supporte `eStorageImage` demandent maintenant `VK_IMAGE_USAGE_STORAGE_BIT` à la création.

### Limite / vérification
- Les texel buffers (`eUniformTexelBuffer` / `eStorageTexelBuffer`) ne sont pas encore implémentés dans cette livraison : leur `VkBufferView` nécessite un chemin de création, de durée de vie et de format qui n'est pas présent dans cette branche.
- Aucune compilation Android/NDK ni exécution sur le S21 n'a été effectuée dans cet environnement.
- `git diff --check` est **OK**. Le patch de livraison est vérifié avec `patch -p1 --dry-run`.
