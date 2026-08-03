package main

import (
	"fmt"
	"strings"
)

type Point struct {
	X int
	Y int
}

func (p Point) magnitude() int {
	return p.X*p.X + p.Y*p.Y
}

func (p Point) scale(k int) Point {
	return Point{p.X * k, p.Y * k}
}

func total(n int) int {
	sum := 0
	for i := 0; i < n; i++ {
		sum += i
	}
	return sum
}

func main() {
	p := Point{1, 2}
	m := p.magnitude()
	q := p.scale(m)
	fmt.Println(total(m), q.X)
	_ = strings.ToUpper("x")
}
