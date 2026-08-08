# Strato Release Publisher

Workflow GitHub Actions pour publier automatiquement les builds Strato dans la section **Releases**.

## 📁 Structure

```
.github/workflows/
├── android_ci.yml      # Build Gradle + upload artifacts
└── release.yml         # Récupère l'artifact + publie en Release
```

## 🚀 Utilisation

### Méthode 1 : Déclenchement manuel (recommandé)

1. Va dans l'onglet **Actions** de ton repo GitHub
2. Clique sur **"Strato Release Publisher"** dans la liste des workflows
3. Clique sur le bouton **"Run workflow"** (en haut à droite)
4. Remplis les options :
   - **build_workflow** : nom du workflow de build (défaut : `Android CI`)
   - **artifact_name** : laisse vide pour auto-détection, ou précise `strato-debug-apk`
   - **prerelease** : coche pour une release candidate
   - **release_title** : titre personnalisé (optionnel)
5. Clique **"Run workflow"**

### Méthode 2 : Automatique après build

Le workflow `release.yml` s'exécute **automatiquement** dès que le workflow `Android CI` réussit sur les branches `main`, `master` ou `dev`.

```yaml
on:
  workflow_run:
    workflows: ["Android CI"]
    types: [completed]
    branches: [main, master, dev]
```

### Méthode 3 : Trigger via API / CLI

```bash
# Via GitHub CLI
gh workflow run release.yml   -f build_workflow="Android CI"   -f prerelease=true   -f release_title="Strato TOTK Preview"

# Via API REST
curl -X POST   -H "Authorization: token $GITHUB_TOKEN"   -H "Accept: application/vnd.github.v3+json"   https://api.github.com/repos/OWNER/REPO/actions/workflows/release.yml/dispatches   -d '{"ref":"main","inputs":{"prerelease":"true"}}'
```

## 📦 Ce qui est publié

| Fichier | Description |
|---------|-------------|
| `*.apk` | L'APK Android de Strato |
| `*.zip` | Archive du build (si présent) |
| `*.so` | Librairies natives (si uploadées) |

## 🏷️ Format des releases

- **Tag** : `v2024.08.08-a1b2c3d` (date + hash court du commit)
- **Titre** : `Strato Build 2024.08.08-a1b2c3d` ou titre personnalisé
- **Pré-release** : Oui par défaut (décocher pour une release stable)
- **Notes** : Générées automatiquement avec les 10 derniers commits

## ⚙️ Prérequis

1. Le workflow de build (`android_ci.yml`) doit produire des **artifacts** avec `actions/upload-artifact`
2. Le repo doit avoir les **permissions** `contents: write` et `actions: read`
3. `GITHUB_TOKEN` est fourni automatiquement par GitHub Actions

## 🔧 Personnalisation

### Changer le chemin de l'APK

Dans `android_ci.yml`, modifie :
```yaml
path: app/build/outputs/apk/debug/*.apk
```

### Changer le format du tag

Dans `release.yml`, modifie la section `Generate version` :
```bash
VERSION="${DATE}-${SHORT_SHA}"
# ou
VERSION="r$(git rev-list --count HEAD)"
```

### Ajouter des fichiers à la release

Dans `release.yml`, ajoute des patterns dans `files:` :
```yaml
files: |
  ./artifacts/**/*.apk
  ./artifacts/**/*.zip
  ./artifacts/**/*.so
  ./artifacts/**/CHANGELOG.md
```

## 🐛 Dépannage

| Problème | Solution |
|----------|----------|
| "Aucun artifact trouvé" | Vérifie que le workflow de build a bien uploadé un artifact avec `actions/upload-artifact` |
| "Permission denied" | Va dans Settings → Actions → General → Workflow permissions → coche "Read and write permissions" |
| "Tag déjà existant" | Le tag est basé sur la date + hash. Si tu relances le même jour avec le même commit, change le format du tag. |
| Release vide | Vérifie que les artifacts contiennent bien des fichiers (pas des dossiers vides) |

## 📜 Licence

GPL-3.0 — même licence que Strato.
