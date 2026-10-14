# -*- coding: utf-8 -*-
"""Genera el logo, el ícono y el fondo de Forja SX2 (mismos tamaños que los originales).
Uso: python logo_forja.py <carpeta ps4/vm/assets del repo>"""
import pathlib
import sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont

FUENTE = "C:/Windows/Fonts/bahnschrift.ttf"   # fuente gruesa y "tecnológica" de Windows


def fuente(tam):
    f = ImageFont.truetype(FUENTE, tam)
    try:
        f.set_variation_by_name("Bold")       # Bahnschrift trae variante negrita
    except Exception:
        pass
    return f


def logo(ancho, alto):
    """'FORJA' plateado + 'SX2' azul con brillo, fondo transparente."""
    img = Image.new("RGBA", (ancho, alto), (0, 0, 0, 0))
    f = fuente(int(alto * 0.55))
    t1, t2 = "FORJA", "SX2"
    d = ImageDraw.Draw(img)
    w1 = d.textlength(t1, font=f)
    w2 = d.textlength(t2, font=f)
    hueco = alto * 0.08
    x = (ancho - (w1 + hueco + w2)) / 2
    y = alto * 0.20
    # Brillo azul detrás de "SX2"
    brillo = Image.new("RGBA", img.size, (0, 0, 0, 0))
    ImageDraw.Draw(brillo).text((x + w1 + hueco, y), t2, font=f, fill=(40, 140, 255, 255))
    img.alpha_composite(brillo.filter(ImageFilter.GaussianBlur(alto * 0.04)))
    # Texto plateado con borde oscuro
    d.text((x, y), t1, font=f, fill=(225, 230, 238, 255), stroke_width=int(alto * 0.012), stroke_fill=(40, 50, 70, 255))
    d.text((x + w1 + hueco, y), t2, font=f, fill=(70, 170, 255, 255), stroke_width=int(alto * 0.012), stroke_fill=(10, 50, 130, 255))
    return img


def fondo(ancho, alto):
    """Degradado azul oscuro con el logo al centro (ícono y pic1)."""
    img = Image.new("RGBA", (ancho, alto))
    d = ImageDraw.Draw(img)
    for yy in range(alto):
        k = yy / alto
        d.line([(0, yy), (ancho, yy)], fill=(int(8 + 10 * k), int(14 + 20 * k), int(35 + 50 * k), 255))
    l = logo(int(ancho * 0.9), int(ancho * 0.9 / 3))
    img.alpha_composite(l, ((ancho - l.width) // 2, (alto - l.height) // 2))
    return img.convert("RGB")


if __name__ == "__main__":
    assets = pathlib.Path(sys.argv[1])
    ui = assets / "ui-png" / "menu_logo.png"
    ui_tam = Image.open(ui).size
    logo(*ui_tam).save(ui)
    for nombre in ("icon0.png", "pic1.png"):
        ruta = assets / "sce_sys" / nombre
        tam = Image.open(ruta).size
        fondo(*tam).save(ruta)
        print(nombre, tam)
    print("menu_logo.png", ui_tam)
