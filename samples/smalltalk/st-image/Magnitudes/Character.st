Character := Magnitude [

    class value: aCodePoint [
        <primitive: CharacterNewPrimitive>
        ^InvalidArgumentError signal
    ]

    class lf [
        ^self value: 10
    ]

    value [
        <primitive: CharacterCodePrimitive>
    ]

    codePoint [
        ^self value
    ]

    = aCharacter [
        ^self value = aCharacter value
    ]

    < aCharacter [
        ^self value < aCharacter value
    ]
]
