#!/usr/bin/env python3
"""Test fit de descripciones OWM en espanol para fuente 8x13."""
FONT8_WIDTH = 8
LCD_WIDTH = 240

def tw(text, scale):
    return len(text) * (FONT8_WIDTH * scale + scale) - scale

# Descripciones OWM reales en espanol (lang=es), verificadas contra API
# OWM usa ASCII sin acentos en espanol
owm = {}

# Group 2xx: Thunderstorm
for c, d in [(200,"tormenta electrica con lluvia ligera"),
(201,"tormenta electrica con lluvia"),
(202,"tormenta electrica con lluvia intensa"),
(210,"tormenta electrica ligera"),
(211,"tormenta electrica"),
(212,"tormenta electrica intensa"),
(221,"tormenta electrica irregular"),
(230,"tormenta electrica con llovizna ligera"),
(231,"tormenta electrica con llovizna"),
(232,"tormenta electrica con llovizna intensa")]:
    owm[c] = d

# Group 3xx: Drizzle
for c, d in [(300,"llovizna de intensidad ligera"),
(301,"llovizna"),
(302,"llovizna de intensidad intensa"),
(310,"llovizna de intensidad ligera"),
(311,"llovizna"),
(312,"llovizna de intensidad intensa"),
(313,"chubascos de lluvia y llovizna"),
(314,"chubascos de lluvia y llovizna intensa"),
(321,"chubascos de llovizna")]:
    owm[c] = d

# Group 5xx: Rain  
for c, d in [(500,"lluvia ligera"),
(501,"lluvia moderada"),
(502,"lluvia intensa"),
(503,"lluvia muy intensa"),
(504,"lluvia extrema"),
(511,"lluvia helada"),
(520,"lluvia intensa de corta duracion"),
(521,"chubascos de lluvia"),
(522,"chubascos de lluvia intensa"),
(531,"chubascos de lluvia irregular")]:
    owm[c] = d

# Group 6xx: Snow
for c, d in [(600,"nevada ligera"),
(601,"nieve"),
(602,"nevada intensa"),
(611,"aguanieve"),
(612,"aguanieve ligera"),
(613,"aguanieve intensa"),
(615,"lluvia y nieve"),
(616,"lluvia y nieve"),
(620,"nevada ligera de corta duracion"),
(621,"chubascos de nieve"),
(622,"chubascos de nieve intensa")]:
    owm[c] = d

# Group 7xx: Atmosphere
for c, d in [(701,"niebla"),
(711,"humo"),
(721,"calina"),
(731,"polvo en suspension"),
(741,"niebla"),
(751,"arena"),
(761,"polvo"),
(762,"ceniza volcanica"),
(771,"turbonada"),
(781,"tornado")]:
    owm[c] = d

# Group 8xx: Clouds
for c, d in [(800,"cielo claro"),
(801,"algo de nubes"),
(802,"nubes dispersas"),
(803,"muy nuboso"),
(804,"nubes")]:
    owm[c] = d

SCALE_TEMP = 2  # 18px/char
SCALE_SENS = 1  # 9px/char
MAX_S2 = LCD_WIDTH // (FONT8_WIDTH * SCALE_TEMP + SCALE_TEMP)  # 13
MAX_S1 = LCD_WIDTH // (FONT8_WIDTH * SCALE_SENS + SCALE_SENS)  # 26

print("MAX chars scale 2 (temp):", MAX_S2)
print("MAX chars scale 1 (sens):", MAX_S1)
print()

# Analisis por prefijo
print("=" * 72)
print("CUANTOS CHARS QUEDAN PARA LA DESCRIPCION (scale 2)")
print("=" * 72)
for label, prefix in [("1 digito (5C )", "5C "),
                      ("2 digitos (20C )", "20C "),
                      ("negativa (-15C )", "-15C ")]:
    pw = tw(prefix, SCALE_TEMP)
    remaining = LCD_WIDTH - pw
    chars = int(remaining // (FONT8_WIDTH * SCALE_TEMP + SCALE_TEMP))
    print(f"  {label}: {pw:>3}px -> {remaining:>3}px libres -> {chars} chars para descripcion")

print()
print("=" * 72)
print("TODAS LAS DESCRIPCIONES OWM (lang=es) — scale 2")
print("=" * 72)
print()

# Ordenar por longitud descendente
items = sorted(owm.items(), key=lambda x: len(x[1]), reverse=True)

PREFIX = "-15C "  # worst case
PREFIX_LEN = len(PREFIX)
max_desc_chars = MAX_S2 - PREFIX_LEN  # 8 chars para descripcion

print(f"{'COD':>4} {'DESCRIPCION':<42} {'TOTAL':>5} {'ANCHO':>6} {'CABE?':>7}  {'TRUNCADO':<20}")
print("-" * 72)

fits_count = 0
for code, desc in items:
    full = PREFIX + desc
    w = tw(full, SCALE_TEMP)
    if w <= LCD_WIDTH:
        fits_count += 1
        fit = "SI"
    else:
        fit = "NO"
    
    # Truncado
    if len(desc) > max_desc_chars:
        truncated = desc[:max_desc_chars] + ".."
    else:
        truncated = desc
    
    print(f"  {code:>4} {desc:<42} {len(full):>5} {w:>5}px  {'X' if w<=LCD_WIDTH else ' '} {fit:>3}  {truncated:<20}")

print()
print(f"Caben: {fits_count}/{len(owm)} con prefijo '{PREFIX}'")
print()

# Con prefijo normal
print("=" * 72)
print("CON PREFIJO NORMAL (20C )", end="")
print()
print("=" * 72)
PREFIX2 = "20C "
fits2 = sum(1 for desc in owm.values() if tw(PREFIX2 + desc, SCALE_TEMP) <= LCD_WIDTH)
print(f"Caben: {fits2}/{len(owm)}")

# Las que no caben ni con prefijo corto
print()
print("=" * 72)
print("LAS QUE NUNCA CABEN (ni con '5C ')", end="")
print()
print("=" * 72)
PREFIX3 = "5C "
never_fit = [(code, desc) for code, desc in owm.items() if tw(PREFIX3 + desc, SCALE_TEMP) > LCD_WIDTH]
for code, desc in sorted(never_fit, key=lambda x: len(x[1]), reverse=True):
    w = tw(PREFIX3 + desc, SCALE_TEMP)
    exceed = w - LCD_WIDTH
    print(f"  [{code:>4}] {desc:<45} {w:>4}px (excede por {exceed:>3}px)")

# Sensacion
print()
print("=" * 72)
print("SENSACION — scale 1 (9px/char) 'Sens: -15C'")
print("=" * 72)
sens_text = "Sens: -15C"
w_s = tw(sens_text, 1)
print(f"  {sens_text} = {w_s}px -> {'CABE' if w_s <= LCD_WIDTH else 'NO CUBE'}")

# La descripcion mas larga truncada en temp
print()
print("=" * 72)
print("CONCLUSION")
print("=" * 72)
longest = max(owm.values(), key=len)
print(f"Descripcion mas larga OWM: '{longest}' ({len(longest)} chars)")
print(f"Display 240px, 8x13 scale 2: max {MAX_S2} chars por linea")
print(f"Con prefijo '{PREFIX}' ({PREFIX_LEN} chars): solo quedan {max_desc_chars} chars para descripcion")
print(f"Desc mas larga truncada: '{longest[:max_desc_chars]}...'")
print()
print("SOLUCIONES:")
print("  A) Escala 1 para la linea de temp (caben 26 chars, casi todo cabe)")
print("  B) Truncar description a lo maximo que quepa")
print("  C) Linea partida: temp en scale 1, desc en scale 1 debajo")
