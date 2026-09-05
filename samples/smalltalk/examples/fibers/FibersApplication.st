FibersApplication := Object [

    run [
        | first second answer |
        first := Fiber fork: [
            Transcript show: 'first starts'; cr.
            Fiber yield.
            20
        ].
        second := Fiber fork: [
            Transcript show: 'second starts'; cr.
            Fiber sleepMilliseconds: 5.
            22
        ].

        answer := first join + second join.
        Transcript show: 'both fibers joined'; cr.
        ^answer
    ]
]
