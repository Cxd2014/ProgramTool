package main

import (
	"fmt"
	"os"
	"bufio"
	"sort"
	"encoding/binary"
	"math/rand"
	"io"
	"time"
)
/*
* 归并排序，随机生成 100000000 个数据放到文件中，
* 然后利用外部排序，并行排序
*/
func main() {

	Init()
	const filename = "large.in"
	const n = 100000000

	// 生成一个随机数文件
	file, err := os.Create(filename)
	if err != nil {
		panic(err)
	}
	defer file.Close()

	p := RandomSource(n)

	writer := bufio.NewWriter(file)
	WriterSink(writer, p)
	writer.Flush()

	p = createPipeline(filename, 8*n, 4)
	fmt.Println("createPipeline done:", time.Now().Sub(startTime))
	
	// 将排序后的结果写入文件
	WriteToFile(p, "large.out")
	fmt.Println("WriteToFile done:", time.Now().Sub(startTime))
	
	printFile("large.out")
	fmt.Println("printFile done:", time.Now().Sub(startTime))

}

/* 打印排序后的文件 */
func printFile(filename string) {
	file, err := os.Open(filename)
	if err != nil {
		panic(err)
	}
	defer file.Close()

	p := ReaderSource(file, -1)
	count := 0
	for v := range p {
		fmt.Println(v)
		count++

		if count > 10 {
			break
		}
	}
}

func createPipeline(filename string, fileSize, chunkCount int) <-chan int {
	chunkSize := fileSize / chunkCount

	// 将文件分成多块
	sortResults := []<-chan int{}
	for i := 0; i < chunkCount; i++ {
		file, err := os.Open(filename)
		if err != nil {
			panic(err)
		}
		
		file.Seek(int64(i * chunkSize), 0)

		// 读取文件内容，放到 chanel 中 
		source := ReaderSource(bufio.NewReader(file), chunkSize)

		// InMenSort 读取chanel的内容放到数组中，然后排序
		sortResults = append(sortResults, InMenSort(source))
	}

	// 将排序后的结果合并
	return MerageN(sortResults...)
}


func WriteToFile(p <-chan int, filename string) {
	file, err := os.Create(filename)
	if err != nil {
		panic(err)
	}
	defer file.Close()

	writer := bufio.NewWriter(file)
	defer writer.Flush()

	WriterSink(writer, p)
}

var startTime time.Time

func Init() {
	startTime = time.Now()
}

func ArraySource(a ...int) <-chan int {
	out := make(chan int, 1024)
	go func() {
		for _, v := range a {
			out <- v
		}
		close(out)
	}()

	return out
}

func InMenSort(in <-chan int) <-chan int {
	out := make(chan int, 1024)
	go func() {
		a := []int{}
		for v := range in {
			a = append(a, v)
		}
		fmt.Println("read done:", time.Now().Sub(startTime))
		sort.Ints(a) // 排序
		fmt.Println("InMenSort done:", time.Now().Sub(startTime))

		for _, v := range a {
			out <- v
		}
		close(out)
	}()

	return out
}

func Merage(in1, in2 <-chan int) <-chan int {
	out := make(chan int, 1024)
	go func() {
		v1, ok1 := <-in1
		v2, ok2 := <-in2

		for ok1 || ok2 {
			if !ok2 || (ok1 && v1 <= v2) {
				out <- v1
				v1, ok1 = <- in1
			} else {
				out <- v2
				v2, ok2 = <- in2
			}
		}
		close(out)
		fmt.Println("Merage done:", time.Now().Sub(startTime))
	}()

	return out
}

func ReaderSource(reader io.Reader, chunkSize int) <-chan int {
	out := make(chan int, 1024)
	go func() {
		buffer := make([]byte, 8)
		bytesRead := 0
		for {
			n, err := reader.Read(buffer)
			bytesRead += n
			if n > 0 {
				v := int(binary.BigEndian.Uint64(buffer))
				out <- v
			}

			if err != nil || (chunkSize != -1 && bytesRead >= chunkSize) {
				break
			}
		}
		fmt.Println("bytesRead:", bytesRead)
		close(out)
	}()

	return out
}

func WriterSink(writer io.Writer, in <-chan int) {
	for v := range in {
		buffer := make([]byte, 8)
		binary.BigEndian.PutUint64(buffer, uint64(v))
		writer.Write(buffer)
	}
}


func RandomSource(count int) <-chan int {
	out := make(chan int, 1024)
	go func() {
		for i := 0; i < count; i++ {
			out <- rand.Int()
		}
		close(out)
	}()

	return out
}

func MerageN(inputs ...<-chan int) <-chan int {
	if len(inputs) == 1 {
		return inputs[0]
	}

	m := len(inputs) / 2
	
	return Merage(MerageN(inputs[:m]...), 
				 MerageN(inputs[m:]...))
}
