ByteArray := ArrayedCollection [
    <shape: indexedUInt8>

    asLatin1String [
        | result index |
        result := String new: self size.
        index := 1.

        [index <= self size] whileTrue: [
            result at: index put: (Character value: (self at: index)).
            index := index + 1
        ].

        ^result
    ]
]
