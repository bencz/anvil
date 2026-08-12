UndefinedObject := Object [

    isNil [
        ^true
    ]

    notNil [
        ^false
    ]

    ifNil: nilBlock [
        ^nilBlock value
    ]

    ifNotNil: notNilBlock [
        ^nil
    ]

    ifNil: nilBlock ifNotNil: notNilBlock [
        ^nilBlock value
    ]

    ifNotNil: notNilBlock ifNil: nilBlock [
        ^nilBlock value
    ]
]
