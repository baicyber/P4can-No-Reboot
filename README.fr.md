# P4can-No-Reboot

> Une petite chose réparée : avec le mode « véhicule banni + restauration TLSSC » actif,
> l'écran véhicule n'a plus besoin d'un redémarrage manuel à chaque mise sous tension.

Firmware prêt à flasher pour la carte Tesla CAN **Waveshare ESP32-P4 WiFi6 3×TWAI**
(32 Mo de flash), basé sur
[P4can-universal-unlock](https://github.com/06066060606060/P4can-universal-unlock)
`v3.29.3`. Les fichiers `.bin` compilés sont à la racine ; l'arborescence source est dans
[`source/`](source/).

**L'interface est en chinois par défaut** ; le sélecteur de langue en haut à droite permet de passer en anglais.

> [!TIP]
> **Envie de voir l'interface sans flasher ?** Une **page simulée** est fournie :
> [`source/sim/p4can-sim.html`](source/sim/p4can-sim.html) — téléchargez-la et ouvrez-la dans un navigateur
> (un clic sur GitHub n'affiche que le code source, c'est normal).
> Elle embarque un faux backend, ne nécessite aucune carte, ne se connecte jamais à une vraie voiture et
> affiche une bannière « SIMULATION » bien visible en haut. Vous pouvez cliquer sur tous les réglages :
> c'est une démo d'interface qui **ne peut modifier aucun véhicule réel**. Les vrais fichiers à flasher
> sont les `.bin` de la racine.

> [!WARNING]
> **Carte 32 Mo de flash uniquement** (Waveshare ESP32-P4-WIFI6 / rev3.2). La table de partitions
> `app5M_fat24M_32MB` ne tient pas sur une carte 16 Mo — flasher cette image sur 16 Mo **rend la carte
> inutilisable**, et il faudra passer par USB pour la récupérer.

---

## Nouveautés (v3.29.3-alight-shift-gpio-rebootfix)

**[中文](README.md) · [English](README.en.md)**

La dernière version est sur la [page Releases](https://github.com/baicyber/P4can-No-Reboot/releases/latest),
tag `v3.29.3-alight-shift-gpio-rebootfix`. Les fichiers `*-tlssc1-*.bin` conservés à la racine sont
l'ancienne version stable de repli.

En plus de l'anti-redémarrage TLSSC, cette version ajoute :

- **Page d'éclairage complètement refondue** : remplacement de l'ancien code par le module officiel
  `lighting` v3.35.8 de l'auteur (stroboscope des feux de route / feux de route manuels / flash des
  feux de brouillard arrière, déclenchement par geste du levier).
- **SmartShift — changement de rapport automatique** : porté depuis la v3.35.8 de l'auteur, désactivé
  par défaut, activable dans les réglages ; confirmation par freinage.
- **Remappage des broches CAN (CAN GPIO)** : modification des broches TX/RX dans les réglages
  (par défaut 20/21, 22/23, 26/27) ; un enregistrement NVS corrompu retombe sur les broches par
  défaut, évitant le « ne se connecte pas » après flashage.
- **Correction anti-redémarrage (rebootfix)** : corrige le redémarrage de l'écran après veille/réveil
  et le redémarrage aléatoire en position P. La replay/echo prend désormais le verrou une seule fois :
  lecture de l'epoch -> vérification du barrage TX -> envoi, supprimant la course avec la tâche
  d'éclairage à 5 ms ; la fenêtre de replay se réarme après veille/réveil ; le Model Y L ne rejoue
  pas la route 0x331 non vérifiée. Test véhicule : aucun redémarrage à la mise sous tension, aucun
  redémarrage aléatoire.
- **Correction de l'état de connexion et de l'interrupteur d'éclairage** : l'ouverture du panneau
  d'éclairage et des autres panneaux ne signale plus faussement « déconnecté » ; l'interrupteur
  d'éclairage s'active et reste activé.
- **Adresse du projet open-source** mise à jour vers `P4can-No-Reboot`.

> [!NOTE]
> Cette version contient du code porté depuis la source interne de l'auteur : **seuls les fichiers
> `.bin` compilés sont publiés, cette source n'est pas rendue publique**. Le correctif minimal
> anti-redémarrage TLSSC reste dans le [PR #15 en amont](https://github.com/06066060606060/P4can-universal-unlock/pull/15).

Voir [Téléchargement](#téléchargement) ci-dessous ; après flashage, vérifiez le numéro de version
affiché en haut de la page.

---

## Le bug corrigé

**Symptôme.** Avec « véhicule banni + restauration TLSSC » actif, il fallait redémarrer manuellement
l'écran à chaque mise sous tension pour retrouver TLSSC.

**Cause.** La restauration TLSSC ne faisait qu'un read-modify-rewrite de `0x331` : la voiture envoie
une trame, nous la modifions. Mais l'écran lit et verrouille sa configuration DAS **une seule fois,
au démarrage**. Si aucune trame `0x331` modifiée n'est présente sur le bus à cet instant, il verrouille
l'état « banni » et seul un redémarrage manuel le fait relire. La documentation du projet Flipper
en amont le signale directement comme « Triggers MCU reboot ».

**Correctif** — avancer la patch avant la lecture de l'écran au démarrage :

| Étape | Action |
|---|---|
| Mise en cache | stocker la charge utile `0x331` patchée dans le NVS (au plus une écriture par cycle, pour préserver la flash) |
| Replay | à CAN B prêt, ouvrir une fenêtre de 60 s et rejouer la trame à 10 Hz sans attendre la voiture |
| Céder la place | si la voiture envoie elle-même `0x331` (trame fraîche sous 2 s), s'arrêter immédiatement et revenir au read-modify-rewrite |
| Terminer | s'arrêter définitivement à l'expiration de la fenêtre ou après 600 trames — jamais de replay infini |

Cela n'agit que lorsque **les deux** réglages « véhicule banni » et « restauration TLSSC » sont actifs ;
sans trame en cache, rien n'est envoyé ; et cela passe par les mêmes barrages TX / suspension admin /
USB passif / epoch que toute autre injection.

**Correctif ultérieur** : l'occasionnel « 1 échec » n'était pas une trame perdue, mais une collision de
verrou TX avec l'injection stroboscope/AP renvoyant `ESP_ERR_TIMEOUT`. C'est maintenant compté comme
un « cède la place » (pas un échec) et retenté au cycle suivant ; le code d'erreur est passé de
`int8_t` à `int16_t` pour ne plus être tronqué.

**Activé par défaut depuis v3.29.3-nr2.** Une installation neuve, comme une mise à jour depuis nr1,
démarrent avec l'anti-redémarrage TLSSC actif. Désactivez-le dans « Plus de fonctions → Anti-redémarrage
TLSSC » si vous ne le souhaitez pas ; ce choix est mémorisé.

**nr3 corrige aussi** l'interrupteur « Nag Killer » de la page d'accueil qui refusait de s'activer
(« Settings could not be saved ») en présence d'un ancien snapshot NAG corrompu — auparavant, seul un
reset complet d'usine permettait de s'en sortir. Un snapshot neuf validé peut désormais écraser la
mauvaise donnée, en effaçant le verrou d'erreur de lecture seulement après une écriture réussie —
**plus besoin de reset**. Un NVS réellement non inscriptible signale toujours une erreur.

**Comment vérifier.** La ligne d'état sous « Plus de fonctions → TLSSC » affiche : désactivé /
prérequis manquant / en attente de la première trame / la voiture envoie / N trames rejouées.
L'API `status.eapNr` expose `enabled`, `prereq`, `cacheValid`, `cacheHex`, `stockRx`, `replayOk`,
`replayFail`, `burst`, `lastErr`.

**Ce qui est garanti :** si notre CAN démarre avant que l'écran ne lise sa configuration, celui-ci
voit le `0x331` `SELF_DRIVING` et n'a pas besoin de redémarrage manuel. Même si l'écran lit plus tard,
la trame en cache reste sur le bus pendant la fenêtre.

**Ce qui n'est pas garanti :** si le `0x331` de ce véhicule est émis en continu (nous cédons alors la
place et passons en read-modify-rewrite) et que l'écran a fini de lire sa configuration avant que
notre CAN ne démarre, ce cycle-là nécessite toujours un redémarrage manuel — c'est une question de
timing du bus, que le logiciel ne peut pas corriger. Jugez sur le véhicule réel avec `stockRx` /
`replayOk` / `cacheHex`.

### Fonctions incluses au passage

Page d'éclairage (v3.35.8 de l'auteur), page « Plus de fonctions », mise à jour OTA dans le navigateur,
relais Wi-Fi, page de capture de trames, interface bilingue (pour l'instant chinois/anglais).

> [!NOTE]
> D'anciennes versions expérimentales `shift1` (remappage CAN GPIO + SmartShift) avaient été retirées
> pour des problèmes de compatibilité de connexion. SmartShift et CAN GPIO sont de retour dans la
> dernière version `v3.29.3-alight-shift-gpio-rebootfix` (les enregistrements GPIO NVS corrompus
> retombent sur les broches par défaut, corrigeant le « ne se connecte pas »). Utilisez la dernière
> version de Releases.

---

## Téléchargement

**Dernière version (recommandée)** — à télécharger sur la
[page Releases](https://github.com/baicyber/P4can-No-Reboot/releases/latest) :

| Fichier | Taille | Usage |
|---|---|---|
| `P4can-unlock-more-v3.29.3-alight-shift-gpio-rebootfix-ota.bin` | 1 521 920 B (1,45 Mo) | **Mises à jour courantes.** Partition app uniquement, rollback en cas d'échec |
| `P4can-unlock-more-v3.29.3-alight-shift-gpio-rebootfix-full.bin` | 33 554 432 B (32 Mo) | **Premier flash / récupération.** Image complète avec bootloader et table de partitions |

Checksums (dernière version, vérifiés) :

| Fichier | sha256 |
|---|---|
| OTA | `51936e140681da03968bfacccaed070b80e7ffd7b780a712db8ea0f5811c0933` |
| full | `b19cb1226bc9c8be74f010600503f4253197b04ac3f626a78387794225f00165` |

**Ancienne version stable de repli** (racine du dépôt) :

| Fichier | Taille | sha256 |
|---|---|---|
| `P4can-unlock-more-v3.29.3-tlssc1-ota.bin` | 1 486 240 B | `4f6afb8309fdb25a6ff7b302d68924d000d9642b37b69ad331d43d81356fd35b` |
| `P4can-unlock-more-v3.29.3-tlssc1-full.bin` | 33 554 432 B | `bb61551f791192d6b1ef16312865e88e5f35bb786014e83dd25c2a074a34a462` |

Vérifiez après téléchargement :

```bash
sha256sum P4can-unlock-more-v3.29.3-alight-shift-gpio-rebootfix-ota.bin
# doit afficher : 51936e14...1c0933
```

---

## Flashage

### Image OTA `*-ota.bin` — mises à jour courantes

1. Connectez téléphone/ordinateur au point d'accès Wi-Fi du P4
2. Ouvrez `http://192.168.4.1` → **Réglages → Mise à jour du firmware**
3. Sélectionnez `P4can-unlock-more-v3.29.3-alight-shift-gpio-rebootfix-ota.bin`
4. Cliquez une fois, cliquez une deuxième fois pour confirmer, attendez la fin de la barre de
   progression et le redémarrage automatique

L'OTA n'écrit que la partition app : **elle ne peut pas modifier bootloader, table de partitions ni
paramètres flash**.

### Image complète `*-full.bin` — premier flash / récupération

```bash
esptool.py --chip esp32p4 -p <PORT> -b 460800 write_flash 0x0 P4can-unlock-more-v3.29.3-alight-shift-gpio-rebootfix-full.bin
```

Layout de l'image complète (ESP32-P4, différent du S3, `0x0` est vide `0xFF`) :

| Décalage | Contenu |
|---|---|
| `0x2000` | bootloader (magic `E9`) |
| `0x8000` | table de partitions (magic `AA50`) |
| `0x10000` | image applicative (magic `E9`) |

Après flashage, vérifiez que la version affichée en haut de la page est
`v3.29.3-alight-shift-gpio-rebootfix`.

---

## Matériel et câblage

Carte : Waveshare ESP32-P4-WIFI6, rev3.2, 32 Mo de flash, partition `app5M_fat24M_32MB`.
`ota_0` / `ota_1` font chacune `0x480000` (4,5 Mo), limite d'un paquet 4 718 592 B.

| Bus | TWAI | TX | RX |
|---|---|---|---|
| CAN 1 BODY | 0 | GPIO20 | GPIO21 |
| CAN 2 CHASSIS | 1 | GPIO22 | GPIO23 |
| CAN 3 PARTY | 2 | GPIO26 | GPIO27 |

---

## Limitations connues

- `gauge` renvoie toujours 503 : les charges utiles 0xB0 (compteur) et 0x5D9 (pression des pneus)
  ne sont pas décodées.
- L'anti-redémarrage TLSSC dépend du timing du bus au démarrage, voir « Ce qui n'est pas garanti ».
- Les trames du stroboscope / feux de brouillard proviennent d'une rétro-ingénierie statique
  (niveau de preuve A), **sans capture réelle sur véhicule** ; validez d'abord les modifications des
  feux de frein dynamiques et des domaines puissance/freinage sur un site fermé.
- La table de partitions, le bootloader et les paramètres flash ne peuvent être modifiés qu'en
  flashage filaire, pas en OTA.
- Toutes les écritures n'ont pas été vérifiées une par une sur un véhicule réel.

---

## Sécurité et conformité

Cet outil est destiné au **débogage sur site fermé**. Modifier le comportement du véhicule
(stroboscope, suppression des notifications, inhibition de la surveillance du conducteur, etc.)
peut être illégal sur la voie publique et peut placer le véhicule hors de sa homologation.

- Vous en assumez la pleine responsabilité, l'auteur décline toute responsabilité
- N'utilisez pas sur la voie publique les fonctions affectant l'éclairage, le klaxon ou la
  reconnaissance des limitations de vitesse
- Avant de flasher, assurez-vous d'avoir un moyen de récupération (image complète + câble USB)

Aucun lien avec Tesla. Tesla, Model 3/Y et autres marques appartiennent à leurs propriétaires.
Les droits et conditions du firmware de base sont dans [NOTICE](NOTICE), ceux des ajouts de ce
projet dans [LICENSE](LICENSE).

---

## ☕ Soutenir

> Le firmware est gratuit. S'il vous a fait gagner du temps, vous pouvez offrir un café à l'auteur
> pour soutenir les prochaines mises à jour~

<table>
  <tr>
    <td align="center"><img src="docs/sponsor/web_alipay.jpg" width="220" alt="QR code Alipay"><br><sub><b>Alipay</b></sub></td>
    <td align="center"><img src="docs/sponsor/web_wechat_reward.jpg" width="220" alt="QR code WeChat Reward"><br><sub><b>Récompense WeChat</b></sub></td>
  </tr>
</table>

## 📮 Contacter l'auteur

> Rapports de problèmes.

<table>
  <tr>
    <td align="center"><img src="docs/sponsor/web_douyin.jpg" width="220" alt="QR code Douyin"><br><sub><b>Douyin · @耶基大学懂事长</b><br>ID <code>yejiuni</code></sub></td>
    <td align="center"><img src="docs/sponsor/web_wechatauthor.jpg" width="220" alt="QR code WeChat de l'auteur"><br><sub><b>WeChat · scannez pour ajouter</b></sub></td>
  </tr>
</table>

---

[中文](README.md) · [English](README.en.md)
