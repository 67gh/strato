#!/bin/bash
# publish_local.sh — Simule la publication de release en local
# Usage: ./publish_local.sh /chemin/vers/lapk.apk [titre]

set -e

APK_PATH="$1"
TITLE="${2:-Strato Local Build}"

if [ -z "$APK_PATH" ]; then
    echo "Usage: $0 /chemin/vers/app-debug.apk [titre]"
    exit 1
fi

if [ ! -f "$APK_PATH" ]; then
    echo "❌ APK non trouvé : $APK_PATH"
    exit 1
fi

VERSION=$(date +'%Y.%m.%d')-$(git rev-parse --short HEAD 2>/dev/null || echo "local")
TAG="v${VERSION}"

echo "=== Strato Local Release ==="
echo "APK     : $APK_PATH"
echo "Version : $VERSION"
echo "Tag     : $TAG"
echo "Titre   : $TITLE"
echo ""

# Créer un dossier de release
mkdir -p "releases/${TAG}"
cp "$APK_PATH" "releases/${TAG}/strato-${VERSION}.apk"

# Générer les notes
cat > "releases/${TAG}/RELEASE_NOTES.md" << EOF
## ${TITLE}

**Date** : $(date +'%d %B %Y')  
**Version** : ${VERSION}  
**Commit** : $(git rev-parse HEAD 2>/dev/null || echo "N/A")

### Fichiers
- \`strato-${VERSION}.apk\`

### Installation
1. Télécharger l'APK
2. Autoriser l'installation de sources inconnues
3. Installer et lancer
EOF

echo "✅ Release locale créée dans : releases/${TAG}/"
echo ""
echo "Pour publier sur GitHub :"
echo "  gh release create ${TAG} releases/${TAG}/*.apk \"
echo "    --title "${TITLE}" \"
echo "    --notes-file releases/${TAG}/RELEASE_NOTES.md \"
echo "    --prerelease"
