Association := Object [

    | key value |

    class key: aKey value: aValue [
        ^self new
            key: aKey value: aValue;
            yourself
    ]

    key: aKey value: aValue [
        key := aKey.
        value := aValue.
        ^self
    ]

    key [
        ^key
    ]

    value [
        ^value
    ]

    value: anObject [
        value := anObject.
        ^anObject
    ]

    = anAssociation [
        (anAssociation isKindOf: Association) ifFalse: [^false].
        ^key = anAssociation key and: [
            value = anAssociation value
        ]
    ]

    hash [
        ^key hash bitXor: value hash
    ]
]
