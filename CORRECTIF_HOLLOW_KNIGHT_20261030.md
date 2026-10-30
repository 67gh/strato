# Hollow Knight — démarrage et composition vidéo — 30 octobre 2026

## Blocage identifié dans les diagnostics

Les cinq rapports envoyés concernent Hollow Knight, sur Samsung SM-A155F / Mali-G57 MC2.
Leur trace de session contient la même exception :

```
Failed to open file: fonts/FontStandard.ttf
```

Aucun dessin, dispatch ou affichage n’est enregistré. L’erreur survient pendant la
construction du service de polices partagé, avant `OS::Execute` et l’exécution du
jeu. Elle apparaît avec le pilote Mali propriétaire et avec PanVK. Ces rapports
ne permettent donc pas de diagnostiquer la cinématique après le lancement.
Ils ne contiennent pas de signal fatal exploitable : la restauration traite
l’erreur de ressource confirmée, sans prétendre expliquer tout le chemin de terminaison.

Le `device.json` identifie le commit `fa96b1d6712fb68b17893aeffd43279bb84ea208`.
La liste du dossier des polices de ce commit a été vérifiée sur GitHub :

- Absentes : `FontStandard.ttf`, `FontChineseSimplified.ttf`, `FontChineseTraditional.ttf`.
- Présentes : `FontExtendedChineseSimplified.ttf`, `FontKorean.ttf`, `FontNintendoExtended.ttf` et `README.md`.

Référence :
https://github.com/67gh/strato/tree/fa96b1d6712fb68b17893aeffd43279bb84ea208/app/src/main/assets/fonts

Les trois fichiers absents existent déjà, complets, dans le `strato.zip` fourni.
Cette livraison les restitue sans modifier leurs octets. Leur présence dans le
ZIP local n’impliquait pas leur présence dans le dépôt ayant construit l’APK.

## Appliquer

1. Copier le contenu du dossier `strato/` de cette archive dans la racine du projet.
2. Ajouter aussi les trois fichiers `.ttf` au dépôt, puis construire un nouvel APK.
   Ils doivent se trouver dans `app/src/main/assets/fonts/`, sans dossier `strato/`
   supplémentaire à l’intérieur du dépôt.
3. Installer l’APK reconstruit et relancer Hollow Knight.

Si Git n’affiche aucune modification pour les polices, vérifier le commit utilisé
par la compilation et la présence des fichiers dans son arbre Git. Le contrôle
ajouté ci-dessous doit passer sur les sources ET sur l’APK généré.

Les autres fichiers de cette archive sont uniquement les modifications et tests
liés à ce correctif. Aucun APK n’est fourni et rien n’a été poussé dans le dépôt.

## Empêcher un nouveau paquet incomplet

`tools/check_shared_fonts.py` vérifie les six noms attendus par le chargeur natif,
les en-têtes SFNT, les bornes des tables et la capacité de la mémoire partagée.
Il rejette notamment un fichier vide, tronqué ou remplacé par un pointeur Git LFS.
Ce contrôle structurel ne remplace pas un moteur de validation typographique.

Les workflows Build et PR Build contrôlent les sources avant compilation.
Le contrôle existant `tools/check_release_apk.py`, déjà exécuté avant envoi et
publication des APK mainline, vérifie maintenant aussi les six polices dans l’APK.
Les APK dev de PR Build sont contrôlés avant leur envoi.
Les builds manuels hors de ces workflows peuvent utiliser :

```
python3 tools/check_shared_fonts.py app/src/main/assets/fonts
python3 tools/check_shared_fonts.py chemin/vers/strato.apk
```

## Optimisation vidéo et référence BOTW

La référence étudiée reste `mamcjr/BOTWRecompAndroid`, notamment
`runtime/nvn/vulkan/exec.c` (blob `f8143124bdf5a441eead44dd4da76a131b4ce207`).
Elle illustre des chemins spécialisés et la conservation de l’ordre des écritures
CPU/GPU. Son moteur NVN n’est pas interchangeable avec le décodeur/compositeur
NVDEC/VIC de Strato. Aucun moteur BOTW ni code spécifique au jeu n’a été importé ici.

Une optimisation locale a été ajoutée au compositeur VIC : à une coordonnée de
texel entière, les trois autres poids du filtrage bilinéaire sont nuls. Le code
lit alors un seul pixel au lieu de quatre. Les matrices de couleur, l’alpha,
les limites, les retournements et le filtrage fractionnaire restent appliqués.
Cela réduit du travail CPU sur les copies vidéo sans redimensionnement. Ce n’est
ni une nouvelle prise en charge de codec, ni une preuve que Hollow Knight utilise
ce chemin pour sa cinématique. Les gains en jeu doivent être mesurés sur l’appareil.

## Vérifications

- 10 tests Python passent, dont la reproduction d’un paquet privé des trois polices,
  l’absence individuelle de chacune des six polices, les fichiers tronqués et
  le dépassement de la mémoire partagée.
- Les scripts de préparation/publication existants passent leurs tests locaux.
- 288 comparaisons complètes avant/après du compositeur passent sous AddressSanitizer
  et UndefinedBehaviorSanitizer : formats RGB/YUV, mémoire linéaire ou par blocs,
  copies entières et agrandissements, retournements, alpha, matrices et remplissage.
- Les tests VIC existants passent : 192 cas complets, 192 cas partiels et 4 cas YUV.
- Banc d’essai CPU séparé : 12 configurations de copie à taille native, 8 images
  de 640×360 par configuration, compilation `-O2`. Temps cumulé dans le compositeur :
  2 717 352 µs avant, 881 798 µs après lors de cette exécution. Les sorties sont identiques.
  Ce résultat ponctuel sur l’hôte n’est pas une mesure de FPS Android ; le filtrage
  fractionnaire ne bénéficie pas du même raccourci et conserve un test de branche.
- Aucun APK construit, aucun test sur téléphone et aucune cinématique de jeu exécutée ici.

Reproduction :

```
python3 tools/diagnostics_tests/shared_fonts.py
python3 tools/updater_tests/publication.py
python3 tools/video_tests/vic_sampling.py --baseline-zip /chemin/vers/strato.zip
python3 tools/video_tests/vic_sampling.py --baseline-zip /chemin/vers/strato.zip --benchmark
```

Le ZIP de référence est celui fourni avant cette livraison, SHA-256 :
`2e22033e6ae26daa05e1aaaa8c2a3f7982139daa9ff94534e2b9f155b11f3631`.
Les tests vidéo utilisent du contenu synthétique et comparent les surfaces complètes.

## Empreintes des trois polices restaurées

- FontStandard.ttf — 2 794 916 octets — SHA-256 `18ee2a734840cef01ae44b72e47540a64c2b1dc9d79ef7a9527fb984b07ad519`
- FontChineseSimplified.ttf — 9 404 852 octets — SHA-256 `2b079a4837f13465312b64107fc272f59ac7c5f33a85328653bd5814bf1f4f5a`
- FontChineseTraditional.ttf — 4 716 656 octets — SHA-256 `9d701a861a2b550b79a9b66fc2c8a5a971468691e5b51044171fa0672fda101a`

Les attributions de ces polices libres sont conservées dans le README du dossier
`app/src/main/assets/fonts/` déjà présent dans le projet.
