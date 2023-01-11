package main

import (
	"encoding/json"
	"fmt"
	"io/ioutil"
	"math"
	"sort"
	"strconv"
	"strings"
	"time"

	"github.com/xuri/excelize/v2"
)

func titleToNumber(s string) int {

	ret := 0
	j := 0
	s = strings.ToUpper(s)
	for i := len(s) - 1; i >= 0; i-- {
		ret = ret + (int(s[i])-'A'+1)*int(math.Pow(float64(26), float64(j)))
		j++
	}

	return ret
}

type SheetInfo struct {
	SheetName     string `json:"sheetName"`
	DataRow       []int  `json:"dataRow,int"`
	StrXiangmu    string `json:"项目"`
	StrDailishang string `json:"代理商"`
	StrWaihu      string `json:"外呼量"`
	StrJietong    string `json:"接通量"`
	StrChengdan   string `json:"成单量"`
	StrDate       string `json:"日期"`
	StrRenwu      string `json:"任务名称"`

	rowStart int
	rowEnd   int

	xiangmu    int
	dailishang int
	waihu      int
	jietong    int
	chengdan   int
	date       int
	renwu      int
}

type FilterConfig struct {
	FileName         string    `json:"fileName"`
	SummarySheetInfo SheetInfo `json:"summarySheetInfo"`
	DetailSheetInfo  SheetInfo `json:"detailSheetInfo"`
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
	fmt.Printf("config:%+v\n", config)

	ssi := &config.SummarySheetInfo
	ssi.rowStart = ssi.DataRow[0] - 1
	ssi.rowEnd = ssi.DataRow[1]
	ssi.xiangmu = titleToNumber(ssi.StrXiangmu) - 1
	ssi.dailishang = titleToNumber(ssi.StrDailishang) - 1
	ssi.waihu = titleToNumber(ssi.StrWaihu) - 1
	ssi.jietong = titleToNumber(ssi.StrJietong) - 1
	ssi.chengdan = titleToNumber(ssi.StrChengdan) - 1

	dsi := &config.DetailSheetInfo
	dsi.rowStart = dsi.DataRow[0] - 1
	dsi.xiangmu = titleToNumber(dsi.StrXiangmu) - 1
	dsi.dailishang = titleToNumber(dsi.StrDailishang) - 1
	dsi.waihu = titleToNumber(dsi.StrWaihu) - 1
	dsi.jietong = titleToNumber(dsi.StrJietong) - 1
	dsi.chengdan = titleToNumber(dsi.StrChengdan) - 1
	dsi.renwu = titleToNumber(dsi.StrRenwu) - 1
	dsi.date = titleToNumber(dsi.StrDate)

	fmt.Printf("init config:%+v\n", config)
	return config, nil
}

type dailiShangInfo struct {
	Name string
	Row  int

	Waihu    int
	Jietong  int
	Chengdan int

	Date string
}

type projectInfo struct {
	Name       string
	Count      int
	DailiShang []dailiShangInfo
}

func excelDateToDate(excelDate string) string {
	excelTime := time.Date(1899, time.December, 30, 0, 0, 0, 0, time.UTC)
	var days, _ = strconv.Atoi(excelDate)
	return excelTime.Add(time.Second * time.Duration(days*86400)).Format("1月2日")
}

func getTableData(processFile *excelize.File, sheet SheetInfo) []projectInfo {
	// 获取 sheet 上所有单元格
	rowArray, err := processFile.GetRows(sheet.SheetName)
	if err != nil {
		fmt.Printf("GetRows error:%v\n", err)
		return nil
	}

	if sheet.rowEnd == 0 {
		sheet.rowEnd = len(rowArray)
	}

	projects := make([]projectInfo, 0)
	for i := sheet.rowStart; i < sheet.rowEnd; i++ {
		if rowArray[i][sheet.xiangmu] != "" {
			projects = append(projects, projectInfo{
				Name:  rowArray[i][sheet.xiangmu],
				Count: 0,
			})
		}
		project := &projects[len(projects)-1]

		date := ""
		if sheet.date != 0 {
			date = excelDateToDate(rowArray[i][sheet.date-1])
		}
		waihu, _ := strconv.Atoi(rowArray[i][sheet.waihu])
		jietong, _ := strconv.Atoi(rowArray[i][sheet.jietong])
		chengdan, _ := strconv.Atoi(rowArray[i][sheet.chengdan])
		project.DailiShang = append(project.DailiShang, dailiShangInfo{
			Name:     rowArray[i][sheet.dailishang],
			Row:      i,
			Waihu:    waihu,
			Jietong:  jietong,
			Chengdan: chengdan,
			Date:     date,
		})

		project.Count++
	}

	return projects
}

func generateDateTable(processFile *excelize.File, sheet SheetInfo) {
	// 获取 sheet 上所有单元格
	rowArray, err := processFile.GetRows(sheet.SheetName)
	if err != nil {
		fmt.Printf("GetRows error:%v\n", err)
		return
	}

	sheet.rowEnd = len(rowArray)
	today := "1月2日" //time.Now().Format("1月2日")
	month := strconv.Itoa(int(time.Now().Month())) + "月"

	fmt.Printf("month:%v today:%v\n", month, today)

	todayRows := make([][]string, 0)

	projectMap := make(map[string]bool)
	companyMap := make(map[string]bool)

	for i := sheet.rowStart; i < sheet.rowEnd; i++ {
		if today == excelDateToDate(rowArray[i][sheet.date-1]) {
			todayRows = append(todayRows, rowArray[i])
			projectMap[rowArray[i][sheet.xiangmu]] = true
			companyMap[rowArray[i][sheet.dailishang]] = true
		}
	}

	projects := make([]string, 0, len(projectMap))
	for p, _ := range projectMap {
		projects = append(projects, p)
	}
	sort.Strings(projects)

	companys := make([]string, 0, len(projectMap))
	for c, _ := range companyMap {
		companys = append(companys, c)
	}
	sort.Strings(companys)

	sortRows := make([][]string, 0)
	totalMap := make(map[string]map[string][4]int)
	for _, project := range projects {
		temp := make(map[string][4]int)
		for _, company := range companys {
			for _, row := range todayRows {
				if row[sheet.xiangmu] == project &&
					row[sheet.dailishang] == company {
					sortRows = append(sortRows, row)
					waihu, _ := strconv.Atoi(row[sheet.waihu])
					jietong, _ := strconv.Atoi(row[sheet.jietong])
					chengdan, _ := strconv.Atoi(row[sheet.chengdan])

					t := temp[row[sheet.dailishang]]
					t[0] = t[0] + waihu
					t[1] = t[1] + jietong
					t[2] = t[2] + chengdan
					t[3] = t[3] + 1
					temp[row[sheet.dailishang]] = t
				}
			}
		}
		totalMap[project] = temp
	}

	fmt.Printf("totalMap: %+v\n", totalMap)

	//设置表头
	processFile.NewSheet(today)
	processFile.MergeCell(today, "A1", "J1")
	processFile.SetSheetRow(today, "A1", &[]interface{}{month + "排产计划-精准系统-" + today})
	processFile.SetSheetRow(today, "A2", &[]interface{}{"项目", "当日外呼的任务名称", "任务包总量", "代理商", "主推的业务包名称", "当日外呼量", "当日接通量", "当日接通率", "当日成单量", "当日外呼成功率"})

	mergeRow := 0
	for i, row := range sortRows {
		current := i + 3
		axis := "A" + strconv.Itoa(current)

		waihu := totalMap[row[sheet.xiangmu]][row[sheet.dailishang]][0]
		jietong := totalMap[row[sheet.xiangmu]][row[sheet.dailishang]][1]
		chengdan := totalMap[row[sheet.xiangmu]][row[sheet.dailishang]][2]

		if current > mergeRow {
			mergeRow = totalMap[row[sheet.xiangmu]][row[sheet.dailishang]][3] + current - 1
			if mergeRow-current > 0 {
				strCurrent := strconv.Itoa(current)
				strMergeRow := strconv.Itoa(mergeRow)
				processFile.MergeCell(today, "F"+strCurrent, "F"+strMergeRow)
				processFile.MergeCell(today, "G"+strCurrent, "G"+strMergeRow)
				processFile.MergeCell(today, "H"+strCurrent, "H"+strMergeRow)
				processFile.MergeCell(today, "I"+strCurrent, "I"+strMergeRow)
				processFile.MergeCell(today, "J"+strCurrent, "J"+strMergeRow)
				fmt.Printf("current:%v mergeRow:%v\n", current, mergeRow)
			}
		}

		interRow := []interface{}{row[sheet.xiangmu], row[sheet.renwu], row[sheet.waihu],
			row[sheet.dailishang], row[sheet.xiangmu], waihu, jietong,
			fmt.Sprintf("%.2f%%\n", (float32(jietong)/float32(waihu))*100), chengdan,
			fmt.Sprintf("%.2f%%\n", (float32(chengdan)/float32(jietong))*100)}
		err := processFile.SetSheetRow(today, axis, &interRow)
		if err != nil {
			fmt.Printf("SetSheetRow err :%v\n", err)
		}
		fmt.Printf("todayRow:%v %v\n", row, axis)
	}
}

// env GOOS=windows GOARCH=amd64 go build -o excel.exe 编译windows可执行文件
func main() {

	config, err := ReadConfigFile()
	if err != nil {
		fmt.Printf("ReadConfigFile error:%v\n", err)
		return
	}

	processFile, err := excelize.OpenFile(config.FileName)
	if err != nil {
		fmt.Printf("OpenFile error:%v\n", err)
		return
	}

	summaryData := getTableData(processFile, config.SummarySheetInfo)
	fmt.Printf("summaryData:%+v\n", summaryData)

	// detailData := getTableData(processFile, config.DetailSheetInfo)
	// fmt.Printf("detailData:%+v\n", detailData)

	generateDateTable(processFile, config.DetailSheetInfo)

	processFile.Save()

	return
}
