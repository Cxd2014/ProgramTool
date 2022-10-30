package main

import (
	"encoding/json"
	"fmt"
	"io/ioutil"
	"os"
	"strconv"
	"time"

	excelize "github.com/xuri/excelize/v2"
)

type Filter struct {
	Row   int      // 需要过滤的行
	Value []string // 已此值来过滤，多个值是 或 的关系
}

type FilterConfig struct {
	FileName      string
	SheetName     string
	Filter        []Filter
	NeedCols      []int // 需要提取的行
	OutFile       string
	ProcessFilter int
	ReadOnlyFile  string
	NeedTxt       int
	OutTxtFiles   []struct {
		newFile  *os.File
		Name     string
		Template string
		Filter   []Filter
	}
}

func ReadConfigFile() (*FilterConfig, error) {

	config := &FilterConfig{}
	bytes, err := ioutil.ReadFile("config.json")
	if err != nil {
		fmt.Printf("read config.json error:%v\n", err)
		return nil, err
	}

	err = json.Unmarshal(bytes, config)
	if err != nil {
		fmt.Printf("Unmarshal config.json error:%v\n", err)
		return nil, err
	}

	return config, nil
}

func DoFilter(filters []Filter, rowArray []string) bool {

	var needRaw bool = false
	for index, filter := range filters {
		col := filter.Row - 1

		var isMatch bool = false
		for _, val := range filter.Value {
			if len(rowArray) > col {
				if rowArray[col] == val {
					isMatch = true
					break
				}
			} else if val == "" {
				isMatch = true
				break
			}
		}

		if isMatch == false {
			break
		} else if (index + 1) == len(filters) {
			needRaw = true
		}
	}

	if len(filters) == 0 { // 没有过滤条件时输出全部行
		needRaw = true
	}

	return needRaw
}

func ProcessTxtFile(rowArray []string, config *FilterConfig) error {

	mapper := func(placeholderName string) string {
		for _, val := range config.NeedCols {
			col := val - 1
			if (strconv.Itoa(val) == placeholderName) &&
				(len(rowArray) > col) {
				return rowArray[col]
			}
		}
		return ""
	}

	for i := range config.OutTxtFiles {
		if DoFilter(config.OutTxtFiles[i].Filter, rowArray) {
			needstr := os.Expand(config.OutTxtFiles[i].Template, mapper) + "\n"
			_, err := config.OutTxtFiles[i].newFile.WriteString(needstr)
			if err != nil {
				fmt.Printf("WriteString error:%v\n", err)
				return err
			}
		}
	}

	return nil
}

func DoProcessFile(rowArray [][]string, config *FilterConfig, streamWriter *excelize.StreamWriter) (int, error) {

	rowLen := len(rowArray)
	writeLine := 1
	readLine := 0
	for readLine < rowLen {

		if DoFilter(config.Filter, rowArray[readLine]) {
			writeLine++
			row := make([]interface{}, len(config.NeedCols))

			for index, val := range config.NeedCols {
				col := val - 1
				if len(rowArray[readLine]) > col {
					row[index] = rowArray[readLine][col]
				}
			}

			writePos, _ := excelize.CoordinatesToCellName(1, writeLine)
			if err := streamWriter.SetRow(writePos, row); err != nil {
				fmt.Printf("SetRow error:%v\n", err)
				return 0, err
			}

			if config.NeedTxt == 1 {
				if err := ProcessTxtFile(rowArray[readLine], config); err != nil {
					fmt.Printf("ProcessTxtFile error:%v\n", err)
				}
			}
		}

		readLine++
	}

	return writeLine, nil

}

var isProcessing int = 1

func Processing(startT time.Time) {
	for {
		time.Sleep(10 * time.Second)
		if isProcessing == 1 {
			fmt.Printf("processing... time:%v\n", time.Since(startT))
		}
	}
}

// env GOOS=windows GOARCH=amd64 go build -o excel.exe 编译windows可执行文件
func main() {

	startT := time.Now()
	config, err := ReadConfigFile()
	if err != nil {
		fmt.Printf("ReadConfigFile error:%v\nafter 30s exit...", err)
		time.Sleep(30 * time.Second)
		return
	}
	fmt.Printf("config:%+v\n", config)

	// 打开要处理的文件
	var fileName string
	if config.ProcessFilter == 1 {
		fileName = config.FileName
	} else {
		fileName = config.ReadOnlyFile
	}
	processFile, err := excelize.OpenFile(fileName)
	if err != nil {
		fmt.Printf("OpenFile error:%v\nafter 30s exit...", err)
		time.Sleep(30 * time.Second)
		return
	}

	defer func() {
		isProcessing = 0
		if err := processFile.Close(); err != nil {
			fmt.Printf("Close error:%v\n", err)
		}

		if config.NeedTxt == 1 {
			for i := range config.OutTxtFiles {
				if config.OutTxtFiles[i].newFile != nil {
					if err := config.OutTxtFiles[i].newFile.Close(); err != nil {
						fmt.Printf("Close error:%v\n", err)
					}
				}
			}
		}

		fmt.Printf("after 60s exit...\n")
		time.Sleep(60 * time.Second)
	}()

	if config.NeedTxt == 1 {
		for i := range config.OutTxtFiles {
			config.OutTxtFiles[i].newFile, err = os.OpenFile(config.OutTxtFiles[i].Name, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0644)
			if err != nil {
				fmt.Printf("Open txt File error:%v\n", err)
				return
			}
		}
	}

	go Processing(startT)
	// 获取 Sheet1 上所有单元格
	rowArray, err := processFile.GetRows(config.SheetName)
	if err != nil {
		fmt.Printf("GetRows error:%v\n", err)
		return
	}

	if len(rowArray) < 2 {
		fmt.Printf("row len less then 2 no need to process\n")
		return
	}

	fmt.Printf("table title:%v\nline1:%v\n", rowArray[0], rowArray[1])
	fmt.Printf("processing... rowLen:%v colLen:%v time:%v\n", len(rowArray), len(rowArray[0]), time.Since(startT))

	if config.ProcessFilter == 1 {
		// 创建一个新表
		newfile := excelize.NewFile()
		streamWriter, err := newfile.NewStreamWriter("Sheet1")
		if err != nil {
			fmt.Printf("NewStreamWriter error:%v\n", err)
			return
		}
		fmt.Printf("creater new file\n")

		styleID, err := newfile.NewStyle(&excelize.Style{Font: &excelize.Font{Color: "#000000"}})
		if err != nil {
			fmt.Printf("NewStyle error:%v\n", err)
			return
		}
		fmt.Printf("set NewStyle\n")

		// 获取标题
		titleRow := make([]interface{}, len(config.NeedCols))
		for index, val := range config.NeedCols {
			col := val - 1
			if len(rowArray[0]) > col {
				titleRow[index] = excelize.Cell{StyleID: styleID, Value: rowArray[0][col]}
			}
		}

		writePos, _ := excelize.CoordinatesToCellName(1, 1)
		if err := streamWriter.SetRow(writePos, titleRow); err != nil {
			fmt.Printf("SetRow error:%v\n", err)
			return
		}
		fmt.Printf("SetRow title\n")

		getRow, err := DoProcessFile(rowArray, config, streamWriter)
		if err != nil {
			fmt.Printf("DoProcessFile error:%v\n", err)
			return
		}
		fmt.Printf("DoProcessFile\n")

		if err := streamWriter.Flush(); err != nil {
			fmt.Printf("Flush error:%v\n", err)
			return
		}
		fmt.Printf("Flush to file\n")

		if err := newfile.SaveAs(config.OutFile); err != nil {
			fmt.Printf("SaveAs error:%v\n", err)
			return
		}

		isProcessing = 0
		fmt.Printf("process done getRow:%v time:%v\n", getRow, time.Since(startT))
	} else {
		fmt.Printf("Not processing!!!!!!!!!!\n")
	}

	return
}
