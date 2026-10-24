// ============================================================
// Pitch - Club de Rellenar Pozos - Ghost in the HAL (FCEyN - UBA)
// 6 slides. Una linea por bullet. El resto se dice hablando.
// Paleta del propio PCB (png limpio de Setup.pdf via pdfimages). Capturas del
// OLED: docs/pics (framebuffer real, KIWIGFX + CRC32) recoloreadas con
// tools/make_assets.py.
// ============================================================

#let paper   = rgb("#F7F5E9")
#let ink     = rgb("#22201A")
#let board   = rgb("#2A2718")
#let sand    = rgb("#EFE7D2")
#let gold    = rgb("#C0AE6C")
#let goldink = rgb("#8A6B2C")
#let muted   = rgb("#6E6857")

#set page(paper: "a4", width: 297mm, height: 210mm, margin: 0pt, fill: paper)
#set text(font: ("Noto Sans", "DejaVu Sans"), size: 11pt, fill: ink, lang: "es")
#set par(leading: 0.8em, justify: false)

#let MONO = ("DejaVu Sans Mono", "Nimbus Mono PS")
#let DISP = ("Noto Sans Display", "Nimbus Sans")

#let marca(c, s: 1.6mm) = box(width: 3.7mm, height: 3.7mm)[
  #place(top + left, rect(width: s, height: s, fill: c))
  #place(top + right, rect(width: s, height: s, fill: c))
  #place(bottom + left, rect(width: s, height: s, fill: c))
  #place(bottom + right, rect(width: s, height: s, fill: c))
]

#let regla(w: 100%, h: 0.9pt, c: gold) = box(width: w, height: h, fill: c)

#let kicker(txt, c: goldink) = text(font: MONO, size: 8.6pt, fill: c, weight: "bold")[#txt]

#let micro(txt, c: goldink) = text(font: MONO, size: 9pt, fill: c)[#txt]

#let celda(txt) = text(font: MONO, size: 9.5pt, fill: ink)[#txt]

#let panel(cuerpo, fondo: paper, borde: gold, relleno: 5mm, ancho: 100%) = box(
  width: ancho, inset: relleno, fill: fondo, radius: 1.2mm, clip: true,
  stroke: (thickness: 0.8pt, paint: borde))[#cuerpo]

#let bullet(tit, cuerpo: none, c: goldink) = grid(
  columns: (4mm, 1fr), column-gutter: 3mm, align: (left + top, left + top),
  [#v(2.4mm)#box(width: 2mm, height: 2mm, fill: c)],
  [
    #text(size: 14pt, fill: ink)[#tit]
    #if cuerpo != none [
      #v(1mm)
      #text(size: 11pt, fill: muted)[#cuerpo]
    ]
  ],
)

#let dato(grande, rotulo, sub: none, c: goldink) = box(width: 100%)[
  #text(font: DISP, size: 32pt, weight: "bold", fill: c)[#grande]
  #v(1mm)
  #text(size: 11.5pt, fill: ink)[#rotulo]
  #if sub != none [
    #v(1mm)
    #text(font: MONO, size: 8.4pt, fill: muted)[#sub]
  ]
]

// teclado radial, a escala de la pantalla real (128x64 -> 88x44 mm)
#let teclado() = box(width: 88mm, height: 44mm, fill: paper, radius: 1mm,
  clip: true, stroke: (thickness: 1.1pt, paint: ink))[
  #place(top + left, dx: 66mm, rect(width: 0.9pt, height: 44mm, fill: ink))
  #place(top + left, dx: 33mm, rect(width: 0.9pt, height: 44mm, fill: ink))
  #place(top + left, dy: 22mm, rect(width: 66mm, height: 0.9pt, fill: ink))
  #place(top + left, dx: 4.5mm, dy: 8mm, text(font: MONO, size: 12pt, fill: ink)[abcdefg])
  #place(top + left, dx: 37mm, dy: 8mm, text(font: MONO, size: 12pt, fill: ink)[hijklmn])
  #place(top + left, dx: 7.5mm, dy: 30mm, text(font: MONO, size: 12pt, fill: ink)[uvwxyz])
  #place(top + left, dx: 39mm, dy: 30mm, text(font: MONO, size: 12pt, fill: ink)[opqrst])
  #place(top + left, dx: 4mm, dy: 1.6mm, text(font: MONO, size: 7.5pt, fill: goldink)[↑])
  #place(top + left, dx: 36.5mm, dy: 1.6mm, text(font: MONO, size: 7.5pt, fill: goldink)[→])
  #place(top + left, dx: 7mm, dy: 23.5mm, text(font: MONO, size: 7.5pt, fill: goldink)[←])
  #place(top + left, dx: 38.5mm, dy: 23.5mm, text(font: MONO, size: 7.5pt, fill: goldink)[↓])
  #place(top + left, dx: 69mm, dy: 6mm, text(font: MONO, size: 12pt, fill: ink)[hola\_])
]

#let slide(kick, titulo, cuerpo, oscuro: false, tsize: 30pt) = {
  let ac = if oscuro { gold } else { goldink }
  let flo = if oscuro { gold } else { muted }
  let fondo = if oscuro { board } else { paper }
  [
    #block(width: 100%, height: 100%, fill: fondo)[
      #grid(
        rows: (auto, 1fr, 12mm),
        columns: 1fr,
        inset: (left: 16mm, right: 16mm, top: 0pt, bottom: 2mm),
        [
          #v(12mm)
          #kicker(kick, c: ac)
          #v(3mm)
          #text(font: DISP, size: tsize, weight: "bold",
                fill: if oscuro { paper } else { ink })[#titulo]
        ],
        [#v(6mm) #regla(c: ac) #v(9mm) #cuerpo],
        [
          #regla(h: 0.6pt, c: ac)
          #v(2mm)
          #grid(
            columns: (auto, 1fr, auto), column-gutter: 4mm,
            align: (left + horizon, left + horizon, left + horizon),
            marca(ac),
            text(font: MONO, size: 8pt, fill: flo)[
              Club de Rellenar Pozos - Ghost in the HAL, FCEyN UBA
            ],
            context text(font: MONO, size: 8pt, fill: flo)[#counter(page).display("01")],
          )
        ],
      )
    ]
  ]
}

// ============================================================
// 1 · Portada
// ============================================================
#slide("GHOST IN THE HAL - FCEyN - UBA - 23 Y 24 OCT 2026",
  "Pager cifrado punto a punto",
  [
    #grid(
      columns: (1fr, 74mm), column-gutter: 12mm, align: (left + horizon, left + top),
      [
        #set par(leading: 1.15em)
        #text(size: 21pt, fill: paper)[
          Red de placas que se mandan mensajes por ESP-NOW encriptadas.
        ]
      ],
      [#align(right + top)[#image("art/kiwi-paper.png", height: 84mm)]],
    )
  ],
  oscuro: true,
)
#pagebreak()


// ============================================================
// 2 · ESP-NOW + E2EE
// ============================================================
#slide("01 - Radio", "Protocolo ESP-NOW", [
  #grid(
    columns: (1fr), column-gutter: 16mm, align: left + top,
    [
      #grid(columns: 1, row-gutter: 6mm, align: left + top,
        bullet("Mensajería punto a punto",
          cuerpo: "sin AP ni router ni wifi"),
        bullet("\"One-to-many y many-to-many\""),
      )
      #v(6mm)
      #micro("github.com/espressif/esp-now", c: muted)
    ],
  )
])
#pagebreak()


// ============================================================
// 2 · ESP-NOW + E2EE
// ============================================================
#slide("2.5 - Brainstorming", "Brainstorming", [
  #grid(
    columns: (1fr), column-gutter: 16mm, align: left + top,
    [
      #grid(columns: 1, row-gutter: 6mm, align: left + top,
        bullet("Nos gustaba el protocolo ESP-NOW y como evita el wifi"),
        bullet("Basamos el proyecto en iterar sobre eso"),
      )
      #v(6mm)
      #micro("github.com/espressif/esp-now", c: muted)
    ],
  )
])
#pagebreak()

// ============================================================
// 3 · El menú
// ============================================================
#slide("02 - Menú y E2EE", "E2EE y Capturas", [
  #grid(
    columns: (1fr, 104mm), column-gutter: 12mm, align: left + top,
    [
          [
      #kicker("COMUNICACION E2EE")
      #v(5mm)
      #grid(columns: 1, row-gutter: 6mm, align: left + top,
        bullet("Encriptado con llave pública y privada",
          cuerpo: "X25519 + AES-256-GCM"),
        bullet("Comunicación por dirección MAC"),
      )
    ],

      #grid(columns: 1, row-gutter: 6.4mm, align: left + top,
        bullet("SNIFFER", cuerpo: "quién es tu par"),
        bullet("CONFIG", cuerpo: "nickname editable, par y huella de la sesión"),
        bullet("MENSAJE", cuerpo: "mensaje borrador"),
      )
    ],
    [
      #grid(columns: (50mm, 50mm), column-gutter: 4mm, row-gutter: 5.5mm,
        align: left + top,
        [#image("oled/A-1-sniffer.png", width: 50mm)
         #v(1.2mm)#micro("SNIFFER", c: muted)],
        [#image("oled/A-2-config.png", width: 50mm)
         #v(1.2mm)#micro("CONFIG", c: muted)],
        [#image("oled/A-3-compose.png", width: 50mm)
         #v(1.2mm)#micro("MENSAJE", c: muted)],
        [#image("oled/B-5c-pager-pagina2.png", width: 50mm)
         #v(1.2mm)#micro("mensaje recibido", c: muted)],
      )
      #v(3mm)
      #micro("capturas del framebuffer", c: muted)
    ],
  )
])
#pagebreak()

// ============================================================
// 4 · Teclado radial
// ============================================================
#slide("03 - Entrada", "Teclado radial", [
  #grid(
    columns: (1fr, 92mm), column-gutter: 12mm, align: left + top,
    [
      #text(size: 13.5pt, fill: ink)[
        Cada dirección de la palanca elige un bloque de letras, recursivamente hasta que queda una sola.
      ]
      #v(7mm)
      #grid(columns: (18mm, 1fr), column-gutter: 4mm, row-gutter: 3mm,
        align: left + horizon,
        text(font: MONO, size: 10.5pt, weight: "bold", fill: goldink)[joystick]
        , text(size: 12pt, fill: ink)[elige el cuadrante],
        text(font: MONO, size: 10.5pt, weight: "bold", fill: goldink)[sw1]
        , text(size: 12pt, fill: ink)[cambiar capa: minúsculas, mayusculas, numeros, símbolos],
        text(font: MONO, size: 10.5pt, weight: "bold", fill: goldink)[sw2]
        , text(size: 12pt, fill: ink)[espacio],
        text(font: MONO, size: 10.5pt, weight: "bold", fill: goldink)[sw3]
        , text(size: 12pt, fill: ink)[borrar],
        text(font: MONO, size: 10.5pt, weight: "bold", fill: goldink)[sw4]
        , text(size: 12pt, fill: ink)[enviar/volver],
      )
      #v(9mm)
    ],
    [
      #image("oled/A-6-teclado.png", width: 88mm)
      #v(3mm)
      #micro("captura real: la capa de minusculas", c: muted)
      #v(7mm)
      #image("image/placa-limpia.png", width: 58mm)
      #v(1.5mm)
    ],
  )
])
#pagebreak()

// ============================================================
// 5 · Extra: voz a texto
// ============================================================
#slide("04 - Extra", "Voz a texto", [
  #grid(
    columns: (1fr, 64mm), column-gutter: 14mm, align: left + top,
    [
      #grid(columns: 1, row-gutter: 6.4mm, align: left + top,
        bullet("Si se conecta a una computadora para correr el modelo"),
        bullet("Se podría reemplazar por una SBC (pesa solo 742 MB!)"),
      )
    ],
    [
      #image("oled/A-4-dictado.png", width: 64mm)
      #v(1.5mm)
      #micro("DICTADO: grabación y nivel del mic", c: muted)
    ],
  )
])
#pagebreak()

// ============================================================
// 6 · Aclaración
// ============================================================
#slide("05 - Disclaimer", "Aclaración", [
  #grid(
    columns: (1fr, 1fr), column-gutter: 16mm, align: left + top,
    [
      #grid(columns: 1, row-gutter: 6mm, align: left + top,
        bullet("Parte del firmware está pseudoprogramada por modelos."),
        bullet("El diseño, la medición y las pruebas van por nuestra cuenta."),
      )
    ],
    [
      #kicker("MODELOS USADOS")
      #v(5mm)
      #grid(columns: 1, row-gutter: 4mm, align: left + top,
        text(font: MONO, size: 13pt, fill: ink)[kimi k3],
        text(font: MONO, size: 13pt, fill: ink)[deepseek v4.1 flash],
        text(font: MONO, size: 13pt, fill: ink)[qwen 3.8 flash next],
      )
    ],
  )
])
