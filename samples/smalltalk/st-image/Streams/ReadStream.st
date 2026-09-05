ReadStream := Stream [
    | collection position |

    class on: elements [
        ^self new initializeOn: elements
    ]

    initializeOn: elements [
        collection := elements.
        position := 0.
        ^self
    ]

    atEnd [
        ^position >= collection size
    ]

    position [
        ^position
    ]

    position: index [
        (index < 0 or: [index > collection size]) ifTrue: [^OutOfRangeError signal].
        position := index.
        ^self
    ]

    reset [
        position := 0.
        ^self
    ]

    skip: count [
        ^self position: position + count
    ]

    peek [
        self atEnd ifTrue: [^OutOfRangeError signal].
        ^collection at: position + 1
    ]

    next [
        | value |
        value := self peek.
        position := position + 1.
        ^value
    ]

    next: count [
        | result |
        (count < 0 or: [position + count > collection size]) ifTrue: [^OutOfRangeError signal].
        result := collection copyFrom: position + 1 to: position + count.
        position := position + count.
        ^result
    ]

    upTo: delimiter [
        | output value |
        output := WriteStream on: (collection copyFrom: 1 to: 0).

        [self atEnd] whileFalse: [
            value := self next.
            value = delimiter ifTrue: [^output contents].
            output nextPut: value
        ].

        ^output contents
    ]
]
