String := ArrayedCollection [

    <shape: indexedUInt8 default: true>
    <shape: indexedUInt16>
    <shape: indexedUInt32>

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
