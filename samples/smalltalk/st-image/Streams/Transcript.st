Transcript := Object [

    class stream [
        ^ExternalStream descriptor: 1
    ]

    class nextPut: aCharacter [
        ^self stream nextPut: aCharacter
    ]

    class nextPutAll: aString [
        ^self stream nextPutAll: aString
    ]

    class lf [
        ^self stream lf
    ]

    class cr [
        ^self stream cr
    ]

    class crlf [
        ^self stream crlf
    ]

    class show: anObject [
        self stream show: anObject.
        ^self
    ]
]
