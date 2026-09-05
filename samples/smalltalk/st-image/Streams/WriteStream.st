WriteStream := Stream [
    | prototype elements |

    class on: collection [
        ^self new initializeOn: collection
    ]

    initializeOn: collection [
        prototype := collection.
        elements := OrderedCollection new: (collection size max: 16).
        ^self
    ]

    nextPut: element [
        elements add: element.
        ^element
    ]

    nextPutAll: collection [
        elements addAll: collection.
        ^collection
    ]

    size [
        ^elements size
    ]

    reset [
        elements := OrderedCollection new.
        ^self
    ]

    contents [
        | result index |
        prototype isString ifTrue: [^String fromCharacters: elements asArray].
        result := prototype class new: elements size.

        (prototype isKindOf: OrderedCollection) ifTrue: [
            result addAll: elements.
            ^result
        ].

        index := 1.

        elements do: [:element |
            result at: index put: element.
            index := index + 1
        ].

        ^result
    ]
]
