SequenceableCollection := Collection [
    <abstract: true>

    , aCollection [
        | result index |
        result := self class new: self size + aCollection size.
        index := 1.
        self do: [ :element |
            result at: index put: element.
            index := index + 1
        ].
        aCollection do: [ :element |
            result at: index put: element.
            index := index + 1
        ].
        ^result
    ]

    first [
        self isEmpty ifTrue: [^OutOfRangeError signal].
        ^self at: 1
    ]

    last [
        self isEmpty ifTrue: [^OutOfRangeError signal].
        ^self at: self size
    ]

    indexOf: anObject ifAbsent: absentBlock [
        ^self
            indexOf: anObject
            startingAt: 1
            ifAbsent: absentBlock
    ]

    indexOf: anObject startingAt: start ifAbsent: absentBlock [
        | index |
        index := start.
        [index <= self size] whileTrue: [
            (self at: index) = anObject ifTrue: [^index].
            index := index + 1
        ].
        ^absentBlock value
    ]

    replaceFrom: first to: last with: source startingAt: sourceFirst [
        | count offset |
        count := last - first + 1.
        count < 0 ifTrue: [^InvalidArgumentError signal].
        sourceFirst < 1 ifTrue: [^OutOfRangeError signal].
        sourceFirst + count - 1 > source size ifTrue: [
            ^OutOfRangeError signal
        ].
        first < 1 ifTrue: [^OutOfRangeError signal].
        last > self size ifTrue: [^OutOfRangeError signal].

        source == self
            ifTrue: [
                first < sourceFirst
                    ifTrue: [
                        offset := 0.
                        [offset < count] whileTrue: [
                            self
                                at: first + offset
                                put: (source at: sourceFirst + offset).
                            offset := offset + 1
                        ]
                    ]
                    ifFalse: [
                        offset := count - 1.
                        [offset >= 0] whileTrue: [
                            self
                                at: first + offset
                                put: (source at: sourceFirst + offset).
                            offset := offset - 1
                        ]
                    ]
            ]
            ifFalse: [
                offset := 0.
                [offset < count] whileTrue: [
                    self
                        at: first + offset
                        put: (source at: sourceFirst + offset).
                    offset := offset + 1
                ]
            ].
        ^source
    ]

    do: aBlock [
        | index |
        index := 1.
        [index <= self size] whileTrue: [
            aBlock value: (self at: index).
            index := index + 1
        ].
        ^self
    ]
]
