Character := Magnitude [

    class value: aCodePoint [
        <primitive: CharacterNewPrimitive>
        ^InvalidArgumentError signal
    ]

    class lf [
        ^self value: 10
    ]

    class cr [
        ^self value: 13
    ]

    class tab [
        ^self value: 9
    ]

    class space [
        ^self value: 32
    ]

    asString [
        | characters |
        characters := Array new: 1.
        characters at: 1 put: self.
        ^String fromCharacters: characters
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
