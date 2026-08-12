ExternalStream := Stream [

    | descriptor |

    class descriptor: anInteger [
        ^self new initializeDescriptor: anInteger
    ]

    class write: descriptor next: byteCount from: aString [
        <primitive: StreamWritePrimitive>
        ^Error signal
    ]

    initializeDescriptor: anInteger [
        descriptor := anInteger.
        ^self
    ]

    descriptor [
        ^descriptor
    ]

    nextPut: aCharacter [
        | string |
        string := String new: 1.
        string at: 1 put: aCharacter.
        self class write: descriptor next: 1 from: string.
        ^aCharacter
    ]

    nextPutAll: aString [
        self class write: descriptor next: aString size from: aString.
        ^aString
    ]
]
