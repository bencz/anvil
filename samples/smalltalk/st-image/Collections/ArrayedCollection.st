ArrayedCollection := SequenceableCollection [
    <abstract: true>

    size [
        ^self basicSize
    ]

    copyFrom: first to: last [
        | result index |
        (first < 1 or: [last < (first - 1) or: [last > self size]]) ifTrue: [^OutOfRangeError signal].
        result := self class new: last - first + 1.
        index := first.

        [index <= last] whileTrue: [
            result at: index - first + 1 put: (self at: index).
            index := index + 1
        ].

        ^result
    ]

    startsWith: prefix [
        | index |
        prefix size > self size ifTrue: [^false].
        index := 1.

        [index <= prefix size] whileTrue: [
            (self at: index) = (prefix at: index) ifFalse: [^false].
            index := index + 1
        ].

        ^true
    ]
]
