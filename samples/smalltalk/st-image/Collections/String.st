String := ArrayedCollection [

    <shape: indexedUInt8 default: true>
    <shape: indexedUInt16>
    <shape: indexedUInt32>

    class fromCharacters: characters [
        <primitive: StringFromCharactersPrimitive>
        ^InvalidArgumentError signal: 'Expected an array containing Unicode Characters'
    ]

    copyFrom: first to: last [
        | characters index |
        (first < 1 or: [last < (first - 1) or: [last > self size]]) ifTrue: [^OutOfRangeError signal].
        characters := Array new: last - first + 1.
        index := first.

        [index <= last] whileTrue: [
            characters at: index - first + 1 put: (self at: index).
            index := index + 1
        ].

        ^String fromCharacters: characters
    ]

    asAsciiLowercase [
        | characters index value |
        characters := Array new: self size.
        index := 1.

        [index <= self size] whileTrue: [
            value := (self at: index) value.
            (value >= 65 and: [value <= 90]) ifTrue: [value := value + 32].
            characters at: index put: (Character value: value).
            index := index + 1
        ].

        ^String fromCharacters: characters
    ]

    trimHttpWhitespace [
        | first last value |
        first := 1.
        last := self size.

        [first <= last] whileTrue: [
            value := (self at: first) value.
            (value = 32 or: [value = 9]) ifFalse: [^self trimHttpWhitespaceFrom: first to: last].
            first := first + 1
        ].

        ^''
    ]

    trimHttpWhitespaceFrom: first to: end [
        | last value |
        last := end.

        [last >= first] whileTrue: [
            value := (self at: last) value.
            (value = 32 or: [value = 9]) ifFalse: [^self copyFrom: first to: last].
            last := last - 1
        ].

        ^''
    ]

    utf8Encoded [
        | bytes value result index |
        bytes := OrderedCollection new: self size.

        self do: [:character |
            value := character value.
            value < 128 ifTrue: [
                bytes add: value
            ] ifFalse: [
                value < 2048 ifTrue: [
                    bytes add: 192 + (value // 64).
                    bytes add: 128 + (value bitAnd: 63)
                ] ifFalse: [
                    value < 65536 ifTrue: [
                        bytes add: 224 + (value // 4096).
                        bytes add: 128 + ((value // 64) bitAnd: 63).
                        bytes add: 128 + (value bitAnd: 63)
                    ] ifFalse: [
                        bytes add: 240 + (value // 262144).
                        bytes add: 128 + ((value // 4096) bitAnd: 63).
                        bytes add: 128 + ((value // 64) bitAnd: 63).
                        bytes add: 128 + (value bitAnd: 63)
                    ]
                ]
            ]
        ].

        result := ByteArray new: bytes size.
        index := 1.
        bytes do: [:byte |
            result at: index put: byte.
            index := index + 1
        ].
        ^result
    ]

    compare: aString [
        <primitive: StringComparePrimitive>
        | count index left right |
        (aString isKindOf: String) ifFalse: [
            ^InvalidArgumentError signal
        ].
        count := self size min: aString size.
        index := 1.
        [index <= count] whileTrue: [
            left := (self at: index) codePoint.
            right := (aString at: index) codePoint.
            left < right ifTrue: [^-1].
            left > right ifTrue: [^1].
            index := index + 1
        ].
        self size < aString size ifTrue: [^-1].
        self size > aString size ifTrue: [^1].
        ^0
    ]

    < aString [
        ^(self compare: aString) < 0
    ]

    <= aString [
        ^(self compare: aString) <= 0
    ]

    > aString [
        ^(self compare: aString) > 0
    ]

    >= aString [
        ^(self compare: aString) >= 0
    ]

    , aCollection [
        <primitive: StringConcatPrimitive>
        ^super , aCollection
    ]

    = aString [
        <primitive: ArrayEqualsPrimitive>
        | index |
        (aString isKindOf: String) ifFalse: [^false].
        self size = aString size ifFalse: [^false].
        index := 1.
        [index <= self size] whileTrue: [
            (self at: index) = (aString at: index) ifFalse: [^false].
            index := index + 1
        ].
        ^true
    ]

    hash [
        <primitive: StringHashPrimitive>
    ]

    asString [
        ^self
    ]

    isString [
        ^true
    ]

    asSymbol [
        <primitive: StringAsSymbolPrimitive>
        ^Error signal
    ]
]
