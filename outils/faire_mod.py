"""Fabrique un paquet de mod .mod : un zip ordinaire sous une autre extension.

Le lanceur tout-en-un (DJFFNY.exe, section Mods) l'installe dans game_files\\mods :

    mod.ini              nom, version, description ; lu par le lanceur, jamais copie
    couverture.png|.jpg  la couverture de la carte du mod (portrait 3:4, 600 x 800 conseille) ;
                         gardee par le lanceur, jamais copiee
    <tout le reste>      copie tel quel dans game_files\\mods (meme arborescence)

    py -3 outils/faire_mod.py <dossier mods> <sortie.mod> --nom "Vendetta" [--version 1.0] [--description "..."]
                              [--couverture image.png|.jpg]

Le dossier donne est le contenu de game_files\\mods (motion\\, assets\\, coreid.txt...).
Un mod contient des fichiers du jeu d'EA : il est pour un usage prive, jamais publie.
"""
import os
import sys
import zipfile


def main():
    a = sys.argv[1:]
    if len(a) < 2:
        raise SystemExit(__doc__)
    src, sortie = os.path.abspath(a[0]), os.path.abspath(a[1])

    def opt(nom, defaut):
        return a[a.index(nom) + 1] if nom in a else defaut
    nom = opt('--nom', os.path.splitext(os.path.basename(sortie))[0])
    ini = '[mod]\nnom=%s\nversion=%s\ndescription=%s\n' % (nom, opt('--version', '1.0'), opt('--description', ''))
    if os.path.exists(os.path.join(src, 'mod.ini')):
        raise SystemExit('%s contient deja un mod.ini' % src)
    couv = opt('--couverture', None)
    ext = os.path.splitext(couv)[1].lower().replace('.jpeg', '.jpg') if couv else ''
    if couv and ext not in ('.png', '.jpg'):
        raise SystemExit('couverture : une image .png ou .jpg')
    n, octets = 0, 0
    tmp = sortie + '.tmp'
    with zipfile.ZipFile(tmp, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        z.writestr('mod.ini', ini.encode('utf-8'))
        if couv:
            z.write(couv, 'couverture' + ext)
        for racine, _, fichiers in os.walk(src):
            for f in sorted(fichiers):
                p = os.path.join(racine, f)
                z.write(p, os.path.relpath(p, src).replace(os.sep, '/'))
                n += 1
                octets += os.path.getsize(p)
    os.replace(tmp, sortie)
    print('%s : %d fichiers, %.1f Mo -> %.1f Mo' % (sortie, n, octets / 1e6, os.path.getsize(sortie) / 1e6))


if __name__ == '__main__':
    main()
